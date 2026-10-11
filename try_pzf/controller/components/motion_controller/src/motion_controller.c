/*
 * Local arm executor. One worker owns BOTH buses, including gesture/single-axis/home.
 * No BSP, recording, console, or position-cache fallback is imported.
 */
#include "motion_controller.h"
#include "motion_controller_config.h"
#include "servo_driver.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#define IDLE_BIT BIT0
#define STOP_WAIT_MS 2500
static const char *TAG="motion_local";
static const uint8_t lx_ids[4]=MOTION_SERVO_LX_IDS;
static const int16_t home[5]={MOTION_HOME_EM3_POS,MOTION_HOME_LX1_POS,
    MOTION_HOME_LX2_POS,MOTION_HOME_LX3_POS,MOTION_HOME_LX5_POS};
atomic_bool s_stop_requested;
atomic_bool s_emergency_stop;
static SemaphoreHandle_t admission_lock, state_lock;
static EventGroupHandle_t idle_event;
static bool initialized, busy, homed;
static motion_action_status_t status=ACTION_STATUS_IDLE;
static char current_action[32];
static esp_err_t worker_error;
static int16_t targets[5];
static bool target_set[5];
static bool position_unconfirmed;
static int16_t saved_pose[5];
static bool saved_valid;

typedef enum {JOB_ACTION, JOB_HOME, JOB_SINGLE, JOB_GESTURE, JOB_RETURN, JOB_APPROACH} job_kind_t;
typedef struct {
    job_kind_t kind;
    char name[32];
    motion_action_params_t params;
    int axis, position, time_ms;
} job_t;

#define SINGLE_QUEUE_CAPACITY 4
static job_t *pending[SINGLE_QUEUE_CAPACITY];
static unsigned pending_count;

/* state_lock held; stop discards queued moves rather than running them later. */
static void clear_pending(void)
{
    for (unsigned i=0; i<pending_count; ++i) free(pending[i]);
    pending_count=0;
}

static bool cancelled(void) { return atomic_load(&s_stop_requested) || atomic_load(&s_emergency_stop); }

bool delay_with_stop_check(uint32_t ms)
{
    while (ms > 0) {
        if (cancelled() || worker_error != ESP_OK) return false;
        uint32_t step=ms > 20 ? 20 : ms;
        vTaskDelay(pdMS_TO_TICKS(step) > 0 ? pdMS_TO_TICKS(step) : 1);
        ms-=step;
    }
    return !cancelled() && worker_error==ESP_OK;
}

static int16_t read_axis(int axis)
{
    return axis==0 ? servo_em3_read_pos(MOTION_SERVO_EM3_ID) : servo_lx_read_pos(lx_ids[axis-1]);
}

static void fail(esp_err_t err)
{
    if (worker_error==ESP_OK && err!=ESP_OK) worker_error=err;
}

static esp_err_t read_pose(int16_t out[5])
{
    for (int i=0; i<5; ++i) {
        if (cancelled()) return ESP_ERR_INVALID_STATE;
        out[i]=read_axis(i);
        if (out[i]==INT16_MIN) {
            ESP_LOGW(TAG,"No valid position reply: axis=%d bus_id=%u",i,
                     (unsigned)(i==0 ? MOTION_SERVO_EM3_ID : lx_ids[i-1]));
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}

static void move_axis(int axis, int16_t position, uint16_t time_ms, uint16_t speed)
{
    if (cancelled() || worker_error!=ESP_OK) return;
    int max=axis==0 ? MOTION_EM3_MAX_POS : MOTION_LX_MAX_POS;
    int min=axis==0 ? MOTION_EM3_MIN_POS : MOTION_LX_MIN_POS;
    if (position < min || position > max) { fail(ESP_ERR_INVALID_ARG); return; }
    esp_err_t ret;
    if (axis==0) {
        ret=servo_em3_enable_torque(MOTION_SERVO_EM3_ID,1);
        if (ret==ESP_OK && !cancelled())
            ret=servo_em3_move(MOTION_SERVO_EM3_ID,position,speed > MOTION_EM3_MAX_SPEED ? MOTION_EM3_MAX_SPEED : speed);
    } else {
        ret=servo_lx_enable_torque(lx_ids[axis-1]);
        if (ret==ESP_OK && !cancelled()) ret=servo_lx_move(lx_ids[axis-1],position,time_ms);
    }
    fail(ret);
    if (ret==ESP_OK && !cancelled()) {
        targets[axis]=position; target_set[axis]=true;
        ESP_LOGI(TAG,"MOVE_TX_OK axis=%d bus_id=%u target=%d (UART sent, position not confirmed)",
            axis,(unsigned)(axis==0 ? MOTION_SERVO_EM3_ID : lx_ids[axis-1]),position);
    } else if (ret!=ESP_OK) {
        ESP_LOGW(TAG,"MOVE_TX_FAILED axis=%d error=%s",axis,esp_err_to_name(ret));
    }
}

void motion_send_pose(const int16_t pos[5], uint16_t time_ms, uint16_t speed)
{
    for (int i=0; i<5; ++i) {
        move_axis(i,pos[i],time_ms,speed);
        if (!delay_with_stop_check(20)) break;
    }
}

static void verify_targets(void)
{
    for (int i=0; i<5 && !cancelled() && worker_error==ESP_OK; ++i) {
        if (!target_set[i]) continue;
        int16_t actual=INT16_MIN;
        int tolerance=i==0 ? 100 : 50;
        int64_t deadline=esp_timer_get_time()+3000000;
        do {
            actual=read_axis(i);
            if (actual!=INT16_MIN && abs((int)actual-targets[i])<=tolerance) break;
            if (!delay_with_stop_check(50)) return;
        } while (esp_timer_get_time()<deadline);
        if (actual==INT16_MIN || abs((int)actual-targets[i]) > tolerance) {
            ESP_LOGW(TAG,"Position verification failed: axis=%d target=%d actual=%d",i,targets[i],actual);
            if (actual==INT16_MIN) {
                position_unconfirmed=true;
                ESP_LOGW(TAG,"MOVE_SENT_POSITION_UNKNOWN axis=%d (not confirmed completed)",i);
            } else {
                fail(ESP_FAIL); /* A valid reply showing the wrong position is a real failure. */
            }
        } else {
            ESP_LOGI(TAG,"Position confirmed: axis=%d target=%d actual=%d",i,targets[i],actual);
        }
    }
}

static motion_action_status_t run_job(job_t *j)
{
    worker_error=ESP_OK;
    memset(target_set,0,sizeof(target_set));
    position_unconfirmed=false;
    ESP_LOGI(TAG,"Action running: %s axis=%d",j->name,j->axis);
    ESP_LOGI(TAG,"PREFLIGHT_BEGIN: no move command sent yet");

    int16_t before[5];
    if (j->kind==JOB_SINGLE || j->kind==JOB_GESTURE) {
        int16_t actual=read_axis(j->axis);
        if (actual==INT16_MIN && !cancelled()) fail(ESP_ERR_TIMEOUT);
    } else {
        esp_err_t ret=read_pose(before);
        if (!cancelled()) fail(ret);
    }
    if (!cancelled() && worker_error!=ESP_OK) {
        ESP_LOGW(TAG,"ACTION_BLOCKED_BEFORE_MOVE name=%s error=%s (no move commands sent)",
                 j->name,esp_err_to_name(worker_error));
    }
    if (!cancelled() && worker_error==ESP_OK) {
        switch(j->kind) {
        case JOB_SINGLE:
        case JOB_GESTURE:
            move_axis(j->axis,j->position,j->time_ms,MOTION_EM3_MAX_SPEED/4);
            delay_with_stop_check(j->time_ms+300);
            break;
        case JOB_HOME:
            motion_send_pose(home,j->time_ms,MOTION_EM3_MAX_SPEED/2);
            delay_with_stop_check(j->time_ms+300);
            break;
        case JOB_RETURN:
            if (!saved_valid) fail(ESP_ERR_NOT_FOUND);
            else { motion_send_pose(saved_pose,1500,MOTION_EM3_MAX_SPEED/3); delay_with_stop_check(1800); }
            break;
        case JOB_APPROACH:
            memcpy(saved_pose,before,sizeof(saved_pose)); saved_valid=true;
            /* Hold the existing curious action's forward keyframe, rather than immediately
             * returning home. go_back restores the measured interaction-before pose. */
            {
                const int16_t forward[5]={MOTION_HOME_EM3_POS+75,MOTION_HOME_LX1_POS,
                    MOTION_HOME_LX2_POS+37,MOTION_HOME_LX3_POS+25,MOTION_HOME_LX5_POS};
                motion_send_pose(forward,1500,MOTION_EM3_MAX_SPEED/3);
                delay_with_stop_check(1800);
            }
            break;
        case JOB_ACTION:
            motion_library_run(j->name,&j->params);
            break;
        }
    }
    if (!cancelled() && worker_error==ESP_OK) {
        delay_with_stop_check(300);
        verify_targets();
    }
    motion_action_status_t result=worker_error!=ESP_OK ? ACTION_STATUS_FAILED :
        cancelled() ? ACTION_STATUS_INTERRUPTED :
        position_unconfirmed ? ACTION_STATUS_UNCONFIRMED : ACTION_STATUS_COMPLETED;
    ESP_LOGI(TAG,"Action %s: %s (%s)",j->name,
        result==ACTION_STATUS_COMPLETED ? "COMPLETED" :
        result==ACTION_STATUS_INTERRUPTED ? "INTERRUPTED" :
        result==ACTION_STATUS_UNCONFIRMED ? "SENT_UNCONFIRMED" : "FAILED",esp_err_to_name(worker_error));
    return result;
}

static void worker(void *arg)
{
    job_t *j=arg;
    for (;;) {
        /* Creation reserves the owner under this lock before the worker starts. */
        xSemaphoreTake(state_lock,portMAX_DELAY);
        status=ACTION_STATUS_RUNNING;
        snprintf(current_action,sizeof(current_action),"%s",j->name);
        xSemaphoreGive(state_lock);
        motion_action_status_t result=run_job(j);
        xSemaphoreTake(state_lock,portMAX_DELAY);
        status=result;
        if (j->kind==JOB_HOME) homed=result==ACTION_STATUS_COMPLETED;
        job_t *next=NULL;
        if (cancelled()) clear_pending();
        if (pending_count) {
            next=pending[0];
            for (unsigned i=1; i<pending_count; ++i) pending[i-1]=pending[i];
            --pending_count;
        } else {
            busy=false; current_action[0]='\0';
            xEventGroupSetBits(idle_event,IDLE_BIT);
        }
        xSemaphoreGive(state_lock);
        free(j);
        if (!next) break;
        j=next;
    }
    vTaskDelete(NULL);
}

/* admission_lock must be held; reserve before creating the worker. */
static esp_err_t submit(job_t *j)
{
    char accepted_name[32];
    snprintf(accepted_name,sizeof(accepted_name),"%s",j->name);
    xSemaphoreTake(state_lock,portMAX_DELAY);
    if (busy) {
        if (j->kind!=JOB_SINGLE || cancelled() || pending_count==SINGLE_QUEUE_CAPACITY) {
            xSemaphoreGive(state_lock);
            ESP_LOGW(TAG,"Action rejected: %s (executor busy or single-axis queue full)",accepted_name);
            free(j); return ESP_ERR_INVALID_STATE;
        }
        unsigned depth=++pending_count;
        pending[depth-1]=j;
        int axis=j->axis;
        xSemaphoreGive(state_lock);
        ESP_LOGI(TAG,"Action queued: %s axis=%d depth=%u (not executed yet)",accepted_name,axis,depth);
        return ESP_OK;
    }
    busy=true; status=ACTION_STATUS_RUNNING;
    snprintf(current_action,sizeof(current_action),"%s",j->name);
    atomic_store(&s_stop_requested,false);
    atomic_store(&s_emergency_stop,false);
    xEventGroupClearBits(idle_event,IDLE_BIT);
    if (xTaskCreate(worker,"motion_action",MOTION_TASK_STACK_SIZE,j,MOTION_TASK_PRIORITY,NULL)!=pdPASS) {
        busy=false; status=ACTION_STATUS_FAILED; current_action[0]='\0';
        xEventGroupSetBits(idle_event,IDLE_BIT);
        xSemaphoreGive(state_lock); free(j); return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(state_lock);
    ESP_LOGI(TAG,"Action accepted: %s (not completed yet)",accepted_name);
    return ESP_OK;
}

static esp_err_t stop_owned(void)
{
    xSemaphoreTake(state_lock,portMAX_DELAY);
    atomic_store(&s_stop_requested,true);
    unsigned dropped=pending_count;
    clear_pending();
    xSemaphoreGive(state_lock);
    if (dropped) ESP_LOGI(TAG,"Stop discarded %u queued single-axis request(s)",dropped);
    EventBits_t bits=xEventGroupWaitBits(idle_event,IDLE_BIT,pdFALSE,pdTRUE,pdMS_TO_TICKS(STOP_WAIT_MS));
    return (bits & IDLE_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t dispatch(job_kind_t kind,const char *name,int axis,int pos,int ms,
                          const motion_action_params_t *params,bool preempt)
{
    if (!initialized) return ESP_ERR_INVALID_STATE;
    job_t *j=calloc(1,sizeof(*j));
    if (!j) return ESP_ERR_NO_MEM;
    j->kind=kind; j->axis=axis; j->position=pos; j->time_ms=ms;
    snprintf(j->name,sizeof(j->name),"%s",name);
    j->params=params ? *params : (motion_action_params_t){.intensity=50,.speed=50,.repeat_count=3,.mode=MOTION_MODE_NORMAL};
    if (j->params.intensity<0 || j->params.intensity>100 || j->params.repeat_count<1 || j->params.repeat_count>10) {
        free(j); return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(admission_lock,pdMS_TO_TICKS(3000))!=pdTRUE) { free(j); return ESP_ERR_TIMEOUT; }
    esp_err_t ret=preempt ? stop_owned() : ESP_OK;
    if (ret==ESP_OK) ret=submit(j); else free(j);
    xSemaphoreGive(admission_lock);
    return ret;
}

esp_err_t motion_controller_init(void)
{
    if (initialized) return ESP_OK;
    admission_lock=xSemaphoreCreateMutex();
    state_lock=xSemaphoreCreateMutex();
    idle_event=xEventGroupCreate();
    if (!admission_lock || !state_lock || !idle_event) {
        if (admission_lock) vSemaphoreDelete(admission_lock);
        if (state_lock) vSemaphoreDelete(state_lock);
        if (idle_event) vEventGroupDelete(idle_event);
        admission_lock=NULL; state_lock=NULL; idle_event=NULL;
        return ESP_ERR_NO_MEM;
    }
    xEventGroupSetBits(idle_event,IDLE_BIT);
    initialized=true;
    ESP_LOGI(TAG,"Executor ready, home=%d/%d/%d/%d/%d; no boot movement",home[0],home[1],home[2],home[3],home[4]);
    return ESP_OK;
}

esp_err_t motion_action_execute_async(const char *name,const motion_action_params_t *params)
{
    if (!name) return ESP_ERR_INVALID_ARG;
    if (!strcmp(name,"home") || !strcmp(name,"init")) return motion_home_async(MOTION_HOME_TIME_MS);
    if (!strcmp(name,"go_back")) return dispatch(JOB_RETURN,name,0,0,0,params,false);
    if (!motion_library_has_action(name)) return ESP_ERR_NOT_SUPPORTED;
    return dispatch(JOB_ACTION,name,0,0,0,params,false);
}
esp_err_t motion_voice_command(const char *command)
{
    if (!command) return ESP_ERR_INVALID_ARG;
    if (!strcmp(command,"come_here")) return dispatch(JOB_APPROACH,command,0,0,0,NULL,false);
    return motion_action_execute_async(command,NULL);
}
esp_err_t motion_home_async(int ms)
{
    if (ms==0) ms=MOTION_HOME_TIME_MS;
    if (ms<0 || ms>65535) return ESP_ERR_INVALID_ARG;
    return dispatch(JOB_HOME,"home",0,0,ms,NULL,false);
}
esp_err_t motion_servo_set_position_async(int axis,int pos)
{
    if (axis<0 || axis>4 || pos<0 || pos>(axis==0 ? MOTION_EM3_MAX_POS : MOTION_LX_MAX_POS))
        return ESP_ERR_INVALID_ARG;
    return dispatch(JOB_SINGLE,"single_axis",axis,pos,1500,NULL,false);
}
esp_err_t motion_gesture_async(const char *gesture)
{
    if (!gesture) return ESP_ERR_INVALID_ARG;
    int pos;
    if (!strcmp(gesture,"thumb_up")) pos=600;
    else if (!strcmp(gesture,"thumb_down")) pos=400;
    else return ESP_ERR_INVALID_ARG;
    return dispatch(JOB_GESTURE,gesture,4,pos,2000,NULL,true);
}
esp_err_t motion_action_stop(void)
{
    if (!initialized) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(admission_lock,pdMS_TO_TICKS(3000))!=pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t ret=stop_owned();
    xSemaphoreGive(admission_lock);
    ESP_LOGI(TAG,"Soft stop: %s (does not unload torque or brake an in-flight servo move)",esp_err_to_name(ret));
    return ret;
}
motion_action_status_t motion_action_get_status(void)
{
    if (!initialized) return ACTION_STATUS_IDLE;
    xSemaphoreTake(state_lock,portMAX_DELAY);
    motion_action_status_t out=status;
    xSemaphoreGive(state_lock);
    return out;
}
esp_err_t motion_controller_get_status(motion_controller_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(state_lock,portMAX_DELAY);
    *out=(motion_controller_status_t){.state=busy ? MOTION_STATE_EXECUTING : MOTION_STATE_READY,
        .mode=MOTION_MODE_NORMAL,.homing_completed=homed,.action_status=status,.current_action=NULL};
    xSemaphoreGive(state_lock);
    return ESP_OK;
}
void motion_log_positions(void)
{
    /* Only called during startup, before the HTTP/MCP entry points exist. */
    for (int i=0; i<5; ++i) {
        int16_t p=read_axis(i);
        if (p==INT16_MIN) ESP_LOGW(TAG,"Startup axis=%d bus_id=%u: no valid reply",i,(unsigned)(i==0 ? 4 : lx_ids[i-1]));
        else ESP_LOGI(TAG,"Startup axis=%d bus_id=%u position=%d",i,(unsigned)(i==0 ? 4 : lx_ids[i-1]),p);
    }
}
