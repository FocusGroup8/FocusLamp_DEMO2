/*
 * Local arm executor. One worker owns all movement on BOTH buses. The read-only
 * observer reserves an idle executor through the same admission lock.
 * No BSP/console is imported; cached positions never authorize a movement.
 */
#include "motion_controller.h"
#include "motion_controller_config.h"
#include "servo_driver.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
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
#define POSITION_SAMPLE_GAP_MS 250
#define POSITION_SAMPLE_STACK_SIZE 4096
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
static motion_position_cache_entry_t position_cache[MOTION_POSITION_AXIS_COUNT];
static bool position_sampling_started;
static motion_calibration_snapshot_t calibration={.reason="idle"};
static uint32_t calibration_sequence;

typedef enum {JOB_ACTION, JOB_HOME, JOB_SINGLE, JOB_GESTURE, JOB_RETURN, JOB_APPROACH, JOB_MANUAL,
              JOB_CALIBRATION_BEGIN, JOB_CALIBRATION_JOG} job_kind_t;
typedef struct {
    job_kind_t kind;
    char name[32];
    motion_action_params_t params;
    int axis, position, time_ms, speed;
    uint32_t session_id;
    int direction;
} job_t;

esp_err_t motion_manual_servo_get_limits(int servo_id,motion_manual_servo_limits_t *out)
{
    if (!out || servo_id<1 || servo_id>MOTION_MANUAL_SERVO_COUNT) return ESP_ERR_INVALID_ARG;
    static const struct { int axis; bool enabled; double min, max; } config[] = {
        {1,MOTION_MANUAL_SERVO_1_ENABLED,MOTION_MANUAL_SERVO_1_MIN_RAW,MOTION_MANUAL_SERVO_1_MAX_RAW},
        {2,MOTION_MANUAL_SERVO_2_ENABLED,MOTION_MANUAL_SERVO_2_MIN_RAW,MOTION_MANUAL_SERVO_2_MAX_RAW},
        {3,MOTION_MANUAL_SERVO_3_ENABLED,MOTION_MANUAL_SERVO_3_MIN_RAW,MOTION_MANUAL_SERVO_3_MAX_RAW},
        {0,MOTION_MANUAL_SERVO_4_ENABLED,MOTION_MANUAL_SERVO_4_MIN_RAW,MOTION_MANUAL_SERVO_4_MAX_RAW},
        {4,MOTION_MANUAL_SERVO_5_ENABLED,MOTION_MANUAL_SERVO_5_MIN_RAW,MOTION_MANUAL_SERVO_5_MAX_RAW},
    };
    unsigned index=(unsigned)(servo_id-1);
    int axis=config[index].axis;
    double min=config[index].min, max=config[index].max;
    int protocol_min=axis==0 ? MOTION_EM3_MIN_POS : MOTION_LX_MIN_POS;
    int protocol_max=axis==0 ? MOTION_EM3_MAX_POS : MOTION_LX_MAX_POS;
    /* Compare before narrowing: NaN, fractions and out-of-int16 values must
     * never turn an enabled but invalid entry into an approved interval. */
    bool valid=config[index].enabled && min>=protocol_min && max<=protocol_max &&
        min>=0 && max<=INT16_MAX && min<=max &&
        min==(int)min && max==(int)max;
    *out=(motion_manual_servo_limits_t){.servo_id=servo_id,.axis=axis,
        .configured=valid,.min_position=valid ? (int)min : 0,
        .max_position=valid ? (int)max : 0};
    return ESP_OK;
}

static esp_err_t manual_target_limits(const job_t *j,motion_manual_servo_limits_t *limits)
{
    esp_err_t ret=motion_manual_servo_get_limits(j->axis==0 ? 4 : lx_ids[j->axis-1],limits);
    if (ret!=ESP_OK) return ret;
    if (!limits->configured) return ESP_ERR_NOT_SUPPORTED;
    return j->position>=limits->min_position && j->position<=limits->max_position ?
        ESP_OK : ESP_ERR_INVALID_ARG;
}

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

static void cache_position(int axis,int16_t raw)
{
    int64_t completed_at_ms=esp_timer_get_time()/1000;
    xSemaphoreTake(state_lock,portMAX_DELAY);
    /* Reply validity is independent of configured command/safety ranges. */
    motion_position_cache_record(&position_cache[axis],raw,axis==0 ? 3000 : 1000,
                                 completed_at_ms);
    xSemaphoreGive(state_lock);
}

static void position_sampler(void *arg)
{
    (void)arg;
    unsigned axis=0;
    for (;;) {
        /* Admission -> state is the same order used by dispatch/stop. Reserve
         * an idle executor for one read call, never for a whole five-axis scan.
         * No state lock is held while a driver read waits/retries. */
        if (xSemaphoreTake(admission_lock,0)==pdTRUE) {
            xSemaphoreTake(state_lock,portMAX_DELAY);
            bool can_sample=!busy;
            xSemaphoreGive(state_lock);
            if (can_sample) {
                int16_t raw=read_axis((int)axis);
                cache_position((int)axis,raw);
                axis=(axis+1U)%MOTION_POSITION_AXIS_COUNT;
            }
            xSemaphoreGive(admission_lock);
        }
        /* Cooldown after completion, not a deadline/catch-up schedule. The
         * current driver call contains up to three attempts. Busy cycles do
         * not reset the cursor, so later axes cannot starve behind axis zero. */
        vTaskDelay(pdMS_TO_TICKS(POSITION_SAMPLE_GAP_MS));
    }
}

esp_err_t motion_controller_start_position_sampling(void)
{
    if (!initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(state_lock,portMAX_DELAY);
    if (position_sampling_started) {
        xSemaphoreGive(state_lock);
        return ESP_OK;
    }
    position_sampling_started=true;
    /* A lower priority does not preempt a UART transaction. The admission
     * reservation prevents a new worker racing an already-started query. */
    UBaseType_t priority=MOTION_TASK_PRIORITY>1 ? MOTION_TASK_PRIORITY-1 : 1;
    if (xTaskCreate(position_sampler,"arm_position",POSITION_SAMPLE_STACK_SIZE,
                    NULL,priority,NULL)!=pdPASS) {
        position_sampling_started=false;
        xSemaphoreGive(state_lock);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(state_lock);
    ESP_LOGI(TAG,"Read-only position sampling started: idle-only, %dms cooldown per axis; no movement",
             POSITION_SAMPLE_GAP_MS);
    return ESP_OK;
}

esp_err_t motion_controller_get_position_snapshot(motion_position_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(state_lock,portMAX_DELAY);
    int64_t now_ms=esp_timer_get_time()/1000;
    out->captured_at_ms=now_ms;
    out->stale_after_ms=MOTION_POSITION_STALE_AFTER_MS;
    out->sampling_started=position_sampling_started;
    out->motion_busy=busy;
    for (unsigned axis=0; axis<MOTION_POSITION_AXIS_COUNT; ++axis) {
        uint8_t bus_id=axis==0 ? MOTION_SERVO_EM3_ID : lx_ids[axis-1];
        out->axes[axis]=motion_position_cache_project(&position_cache[axis],
            (uint8_t)axis,bus_id,now_ms,MOTION_POSITION_STALE_AFTER_MS);
    }
    xSemaphoreGive(state_lock);
    return ESP_OK;
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

static bool calibration_current(const job_t *j,motion_calibration_config_t *config)
{
    xSemaphoreTake(state_lock,portMAX_DELAY);
    bool current=calibration.active && calibration.session_id==j->session_id;
    if (current && config) *config=calibration.config;
    xSemaphoreGive(state_lock);
    return current;
}

static void calibration_observe(const job_t *j,int16_t actual)
{
    cache_position(j->axis,actual);
    if (actual<0 || actual>(j->axis==0 ? 3000 : 1000)) return;
    xSemaphoreTake(state_lock,portMAX_DELAY);
    if (calibration.active && calibration.session_id==j->session_id) {
        if (!calibration.has_observations) {
            calibration.observed_min=actual; calibration.observed_max=actual;
            calibration.has_observations=true;
        } else {
            if (actual<calibration.observed_min) calibration.observed_min=actual;
            if (actual>calibration.observed_max) calibration.observed_max=actual;
        }
    }
    xSemaphoreGive(state_lock);
}

static motion_action_status_t calibration_result(const job_t *j,motion_action_status_t result,
                                                  esp_err_t error,const char *reason,bool ready)
{
    xSemaphoreTake(state_lock,portMAX_DELAY);
    if (calibration.session_id==j->session_id) {
        /* An end/stop which raced a read or write always wins over its result. */
        if (!calibration.active || cancelled()) {
            result=ACTION_STATUS_INTERRUPTED; error=ESP_ERR_INVALID_STATE;
            reason="cancelled"; ready=false;
        }
        calibration.ready=calibration.active && ready;
        calibration.action_status=result; calibration.last_error=error;
        calibration.reason=reason;
    }
    xSemaphoreGive(state_lock);
    ESP_LOGI(TAG,"Calibration session=%u axis=%d reason=%s error=%s",
             (unsigned)j->session_id,j->axis,reason,esp_err_to_name(error));
    return result;
}

static motion_action_status_t run_calibration_job(job_t *j)
{
    motion_calibration_config_t config;
    if (cancelled() || !calibration_current(j,&config))
        return calibration_result(j,ACTION_STATUS_INTERRUPTED,ESP_ERR_INVALID_STATE,"session_changed",false);

    /* The reference and each step use a new bus read, never the idle cache. */
    int16_t actual=read_axis(j->axis);
    calibration_observe(j,actual);
    if (cancelled() || !calibration_current(j,NULL))
        return calibration_result(j,ACTION_STATUS_INTERRUPTED,ESP_ERR_INVALID_STATE,"cancelled",false);
    if (actual<0 || actual>(j->axis==0 ? 3000 : 1000))
        return calibration_result(j,ACTION_STATUS_FAILED,ESP_ERR_TIMEOUT,"read_failed",false);

    if (j->kind==JOB_CALIBRATION_BEGIN) {
        xSemaphoreTake(state_lock,portMAX_DELAY);
        if (calibration.active && calibration.session_id==j->session_id) {
            calibration.reference_raw=actual; calibration.reference_valid=true;
        }
        xSemaphoreGive(state_lock);
        if (actual<config.min_position || actual>config.max_position)
            return calibration_result(j,ACTION_STATUS_FAILED,ESP_ERR_INVALID_STATE,"current_outside",false);
        return calibration_result(j,ACTION_STATUS_COMPLETED,ESP_OK,"ready",true);
    }

    int target;
    motion_calibration_target_result_t decision=motion_calibration_target(&config,actual,j->direction,&target);
    if (decision==MOTION_CALIBRATION_CURRENT_OUTSIDE)
        return calibration_result(j,ACTION_STATUS_FAILED,ESP_ERR_INVALID_STATE,"current_outside",false);
    if (decision!=MOTION_CALIBRATION_TARGET_OK)
        return calibration_result(j,ACTION_STATUS_FAILED,ESP_ERR_INVALID_ARG,"trial_boundary",true);

    xSemaphoreTake(state_lock,portMAX_DELAY);
    bool current=calibration.active && calibration.session_id==j->session_id && !cancelled();
    if (current) { calibration.has_target=true; calibration.target_raw=target; }
    xSemaphoreGive(state_lock);
    if (!current)
        return calibration_result(j,ACTION_STATUS_INTERRUPTED,ESP_ERR_INVALID_STATE,"cancelled",false);

    /* Session ownership was rechecked after the read. move_axis also checks
     * stop both before torque-on and again before transmitting the target. */
    move_axis(j->axis,(int16_t)target,(uint16_t)config.time_ms,(uint16_t)config.speed);
    if (worker_error!=ESP_OK)
        return calibration_result(j,ACTION_STATUS_FAILED,worker_error,"move_failed",false);
    if (!delay_with_stop_check(j->axis==0 ? 300U : (uint32_t)config.time_ms+300U))
        return calibration_result(j,ACTION_STATUS_INTERRUPTED,ESP_ERR_INVALID_STATE,"cancelled",false);

    /* Exact raw agreement is intentional here: the legacy verifier's 50/100
     * raw tolerance could claim that a small calibration step never moved.
     * The deadline prevents another query from starting after it; it cannot
     * interrupt a driver read already performing its internal retry sequence. */
    int64_t deadline=esp_timer_get_time()+3000000;
    do {
        if (cancelled() || !calibration_current(j,NULL))
            return calibration_result(j,ACTION_STATUS_INTERRUPTED,ESP_ERR_INVALID_STATE,"cancelled",false);
        actual=read_axis(j->axis);
        calibration_observe(j,actual);
        if (cancelled() || !calibration_current(j,NULL))
            return calibration_result(j,ACTION_STATUS_INTERRUPTED,ESP_ERR_INVALID_STATE,"cancelled",false);
        if (actual>=0 && actual<=(j->axis==0 ? 3000 : 1000)) {
            if (actual<config.min_position || actual>config.max_position)
                return calibration_result(j,ACTION_STATUS_FAILED,ESP_ERR_INVALID_STATE,"current_outside",false);
            if (actual==target)
                return calibration_result(j,ACTION_STATUS_COMPLETED,ESP_OK,"step_complete",true);
        }
        if (!delay_with_stop_check(50))
            return calibration_result(j,ACTION_STATUS_INTERRUPTED,ESP_ERR_INVALID_STATE,"cancelled",false);
    } while (esp_timer_get_time()<deadline);
    return calibration_result(j,ACTION_STATUS_UNCONFIRMED,ESP_ERR_TIMEOUT,"position_unconfirmed",false);
}

static motion_action_status_t run_job(job_t *j)
{
    worker_error=ESP_OK;
    memset(target_set,0,sizeof(target_set));
    position_unconfirmed=false;
    ESP_LOGI(TAG,"Action running: %s axis=%d",j->name,j->axis);
    ESP_LOGI(TAG,"PREFLIGHT_BEGIN: no move command sent yet");

    if (j->kind==JOB_CALIBRATION_BEGIN || j->kind==JOB_CALIBRATION_JOG)
        return run_calibration_job(j);

    int16_t before[5];
    if (j->kind==JOB_MANUAL) {
        motion_manual_servo_limits_t limits;
        fail(manual_target_limits(j,&limits));
        if (!cancelled() && worker_error==ESP_OK) {
            int16_t actual=read_axis(j->axis); /* Fresh target-axis read; cache is not authority. */
            cache_position(j->axis,actual);
            if (actual==INT16_MIN) fail(ESP_ERR_TIMEOUT);
            else if (actual<limits.min_position || actual>limits.max_position)
                fail(ESP_ERR_INVALID_STATE);
        }
    } else if (j->kind==JOB_SINGLE || j->kind==JOB_GESTURE) {
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
        case JOB_CALIBRATION_BEGIN:
        case JOB_CALIBRATION_JOG:
            break; /* Handled above; never falls through to a normal action. */
        case JOB_MANUAL:
            move_axis(j->axis,j->position,j->time_ms,j->speed);
            /* EM3's speed is not a duration; arrival is checked by the shared
             * bounded verifier. LX alone supplies a movement duration. */
            if (j->axis!=0) delay_with_stop_check(j->time_ms+300);
            break;
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
    bool calibration_job=j->kind==JOB_CALIBRATION_BEGIN || j->kind==JOB_CALIBRATION_JOG;
    if ((calibration.active && !calibration_job) ||
        (calibration_job && (!calibration.active || calibration.session_id!=j->session_id))) {
        xSemaphoreGive(state_lock); free(j); return ESP_ERR_INVALID_STATE;
    }
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
    if (calibration.active) {
        calibration.active=false; calibration.ready=false;
        calibration.action_status=ACTION_STATUS_INTERRUPTED;
        calibration.last_error=ESP_ERR_INVALID_STATE; calibration.reason="cancelled";
    }
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
    /* Reject before a preempting gesture could stop the calibration worker. */
    xSemaphoreTake(state_lock,portMAX_DELAY);
    bool calibration_locked=calibration.active;
    xSemaphoreGive(state_lock);
    if (calibration_locked) {
        xSemaphoreGive(admission_lock); free(j); return ESP_ERR_INVALID_STATE;
    }
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
    calibration_sequence=esp_random();
    if (!calibration_sequence) calibration_sequence=1;
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

esp_err_t motion_manual_servo_async(int servo_id,int pos,int time_ms,int speed)
{
    motion_manual_servo_limits_t limits;
    esp_err_t ret=motion_manual_servo_get_limits(servo_id,&limits);
    if (ret!=ESP_OK) return ret;
    if (!limits.configured) return ESP_ERR_NOT_SUPPORTED;
    if (pos<limits.min_position || pos>limits.max_position) return ESP_ERR_INVALID_ARG;
    if (limits.axis==0) {
        if (time_ms!=0) return ESP_ERR_INVALID_ARG;
        if (speed==0) speed=MOTION_MANUAL_SPEED_DEFAULT;
        if (speed<MOTION_MANUAL_SPEED_MIN || speed>MOTION_MANUAL_SPEED_MAX) return ESP_ERR_INVALID_ARG;
    } else {
        if (speed!=0) return ESP_ERR_INVALID_ARG;
        if (time_ms==0) time_ms=MOTION_MANUAL_TIME_MS_DEFAULT;
        if (time_ms<MOTION_MANUAL_TIME_MS_MIN || time_ms>MOTION_MANUAL_TIME_MS_MAX) return ESP_ERR_INVALID_ARG;
    }
    if (!initialized) return ESP_ERR_INVALID_STATE;
    job_t *j=calloc(1,sizeof(*j));
    if (!j) return ESP_ERR_NO_MEM;
    j->kind=JOB_MANUAL; j->axis=limits.axis; j->position=pos;
    j->time_ms=time_ms; j->speed=speed;
    snprintf(j->name,sizeof(j->name),"manual_servo_%d",servo_id);
    /* Reserve through the existing sampler/stop admission lock. A concurrent
     * reservation or busy worker rejects this request; JOB_MANUAL never queues. */
    if (xSemaphoreTake(admission_lock,0)!=pdTRUE) { free(j); return ESP_ERR_TIMEOUT; }
    ret=submit(j);
    xSemaphoreGive(admission_lock);
    return ret;
}

esp_err_t motion_calibration_begin_async(const motion_calibration_config_t *config,uint32_t *session_id)
{
    if (!session_id) return ESP_ERR_INVALID_ARG;
    *session_id=0;
    int axis=config ? motion_calibration_axis(config->servo_id) : -1;
    int protocol_min=axis==0 ? MOTION_EM3_MIN_POS : MOTION_LX_MIN_POS;
    int protocol_max=axis==0 ? MOTION_EM3_MAX_POS : MOTION_LX_MAX_POS;
    if (axis<0 || !motion_calibration_config_valid(config,protocol_min,protocol_max))
        return ESP_ERR_INVALID_ARG;
    if (!initialized) return ESP_ERR_INVALID_STATE;
    job_t *j=calloc(1,sizeof(*j));
    if (!j) return ESP_ERR_NO_MEM;
    if (xSemaphoreTake(admission_lock,0)!=pdTRUE) { free(j); return ESP_ERR_TIMEOUT; }
    xSemaphoreTake(state_lock,portMAX_DELAY);
    if (calibration.active || busy || pending_count) {
        xSemaphoreGive(state_lock); xSemaphoreGive(admission_lock);
        free(j); return ESP_ERR_INVALID_STATE;
    }
    if (++calibration_sequence==0) ++calibration_sequence;
    uint32_t id=calibration_sequence;
    calibration=(motion_calibration_snapshot_t){.active=true,.session_id=id,.config=*config,
        .action_status=ACTION_STATUS_RUNNING,.last_error=ESP_OK,.reason="opening"};
    xSemaphoreGive(state_lock);
    j->kind=JOB_CALIBRATION_BEGIN; j->axis=axis; j->session_id=id;
    snprintf(j->name,sizeof(j->name),"calibration_begin_%d",config->servo_id);
    esp_err_t ret=submit(j);
    if (ret==ESP_OK) *session_id=id;
    else {
        xSemaphoreTake(state_lock,portMAX_DELAY);
        calibration.active=false; calibration.ready=false;
        calibration.action_status=ACTION_STATUS_FAILED; calibration.last_error=ret;
        calibration.reason="submit_failed";
        xSemaphoreGive(state_lock);
    }
    xSemaphoreGive(admission_lock);
    return ret;
}

esp_err_t motion_calibration_jog_async(uint32_t session_id,int direction)
{
    if (!session_id || (direction!=-1 && direction!=1)) return ESP_ERR_INVALID_ARG;
    if (!initialized) return ESP_ERR_INVALID_STATE;
    job_t *j=calloc(1,sizeof(*j));
    if (!j) return ESP_ERR_NO_MEM;
    if (xSemaphoreTake(admission_lock,0)!=pdTRUE) { free(j); return ESP_ERR_TIMEOUT; }
    xSemaphoreTake(state_lock,portMAX_DELAY);
    if (!calibration.active || calibration.session_id!=session_id) {
        xSemaphoreGive(state_lock); xSemaphoreGive(admission_lock);
        free(j); return ESP_ERR_NOT_FOUND;
    }
    if (!calibration.ready || busy || pending_count) {
        xSemaphoreGive(state_lock); xSemaphoreGive(admission_lock);
        free(j); return ESP_ERR_INVALID_STATE;
    }
    j->kind=JOB_CALIBRATION_JOG; j->axis=motion_calibration_axis(calibration.config.servo_id);
    j->session_id=session_id; j->direction=direction;
    calibration.has_target=false; calibration.action_status=ACTION_STATUS_RUNNING;
    calibration.last_error=ESP_OK; calibration.reason="stepping";
    xSemaphoreGive(state_lock);
    snprintf(j->name,sizeof(j->name),"calibration_jog_%d",j->axis);
    esp_err_t ret=submit(j);
    if (ret!=ESP_OK) {
        xSemaphoreTake(state_lock,portMAX_DELAY);
        calibration.ready=false; calibration.action_status=ACTION_STATUS_FAILED;
        calibration.last_error=ret; calibration.reason="submit_failed";
        xSemaphoreGive(state_lock);
    }
    xSemaphoreGive(admission_lock);
    return ret;
}

esp_err_t motion_calibration_end(uint32_t session_id)
{
    if (!session_id) return ESP_ERR_INVALID_ARG;
    if (!initialized) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(admission_lock,pdMS_TO_TICKS(3000))!=pdTRUE) return ESP_ERR_TIMEOUT;
    xSemaphoreTake(state_lock,portMAX_DELAY);
    bool matches=calibration.session_id==session_id;
    bool active=calibration.active;
    xSemaphoreGive(state_lock);
    /* A repeated end for the old inactive report must not stop normal work. */
    esp_err_t ret=!matches ? ESP_ERR_NOT_FOUND : active ? stop_owned() : ESP_OK;
    xSemaphoreGive(admission_lock);
    return ret;
}

esp_err_t motion_calibration_get_snapshot(motion_calibration_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(state_lock,portMAX_DELAY);
    *out=calibration;
    out->motion_busy=busy;
    int axis=motion_calibration_axis(calibration.config.servo_id);
    if (axis>=0) {
        uint8_t bus_id=axis==0 ? MOTION_SERVO_EM3_ID : lx_ids[axis-1];
        out->feedback=motion_position_cache_project(&position_cache[axis],(uint8_t)axis,bus_id,
            esp_timer_get_time()/1000,MOTION_POSITION_STALE_AFTER_MS);
    }
    xSemaphoreGive(state_lock);
    return ESP_OK;
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
        cache_position(i,p);
        if (p==INT16_MIN) ESP_LOGW(TAG,"Startup axis=%d bus_id=%u: no valid reply",i,(unsigned)(i==0 ? 4 : lx_ids[i-1]));
        else ESP_LOGI(TAG,"Startup axis=%d bus_id=%u position=%d",i,(unsigned)(i==0 ? 4 : lx_ids[i-1]),p);
    }
}
