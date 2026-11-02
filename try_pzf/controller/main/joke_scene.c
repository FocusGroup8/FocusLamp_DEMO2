#include "joke_scene.h"
#include "joke_scene_policy.h"
#include "motion_controller.h"
#include "mipi_dsi_bridge.h"
#include "controller_modes.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdio.h>

static QueueHandle_t events;
static const char *TAG="JOKE_SCENE";
static void note(int event)
{
    if (events && xQueueSend(events,&event,0)!=pdTRUE) {
        xQueueReset(events);
        int cancel=JOKE_OTHER;
        xQueueSend(events,&cancel,0);
        ESP_LOGW(TAG,"Event queue full; pending joke cancelled");
    }
}
void joke_scene_note_user(const char *text) { note(joke_scene_requested(text) ? JOKE_REQUEST : JOKE_OTHER); }
void joke_scene_note_tts_start(void) { note(JOKE_TTS_START); }
void joke_scene_note_sentence(void) { note(JOKE_SENTENCE); }
void joke_scene_note_end(void) { note(JOKE_END); }
void joke_scene_note_vision_accepted(void) { note(SCENE_VISION_ACCEPTED); }
void joke_scene_note_companion_ended(void) { note(SCENE_COMPANION_ENDED); }
void joke_scene_note_distraction(void) { note(SCENE_DISTRACTION); }

static const char *business_action(int event)
{
    controller_modes_status_t mode;
    if (controller_modes_get_status(&mode)!=ESP_OK) return NULL;
    return scene_business_action(event,mode.mode==APP_STATE_FOCUS,mode.mode==APP_STATE_IDLE);
}

static void worker(void *arg)
{
    (void)arg;
    joke_scene_policy_t policy={0};
    bool active=false, expression_owned=false;
    int64_t finish_by=0, renew_at=0, last_distraction=-60000;
    const char *active_expression="happy";
    char previous[24]="neutral";
    for (;;) {
        int event, business=0;
        if (xQueueReceive(events,&event,pdMS_TO_TICKS(100))==pdTRUE) {
            do {
                int64_t now=esp_timer_get_time()/1000;
                if (event>=SCENE_VISION_ACCEPTED) {
                    if (event!=SCENE_DISTRACTION || scene_distraction_due(now,last_distraction)) {
                        if (event==SCENE_DISTRACTION) last_distraction=now;
                        business=event;
                    }
                } else joke_scene_event(&policy,event,now);
            } while (xQueueReceive(events,&event,0)==pdTRUE);
        }
        int64_t now=esp_timer_get_time()/1000;
        bool joke=policy.fire;
        if (joke || business) {
            policy.fire=false;
            const char *action=joke ? "happy" : business_action(business);
            motion_controller_status_t state;
            if (!active && action && (!joke || (policy.speaking && now<=policy.expires_ms)) &&
                motion_controller_get_status(&state)==ESP_OK && state.state==MOTION_STATE_READY) {
                const char *expression=!strcmp(action,"curious") ? "surprised" : action;
                bool held=false;
                esp_err_t ret=mipi_dsi_bridge_expression_get(previous,sizeof(previous),&held);
                if (business==SCENE_COMPANION_ENDED && !joke)
                    snprintf(previous,sizeof(previous),"neutral");
                if (ret==ESP_OK && !held) ret=mipi_dsi_bridge_expression_set(expression,15000);
                else if (ret==ESP_OK) ret=ESP_ERR_INVALID_STATE;
                /* Discard business events arriving during HTTP; never backlog movement. */
                while (xQueueReceive(events,&event,0)==pdTRUE)
                    if (event<SCENE_VISION_ACCEPTED) joke_scene_event(&policy,event,esp_timer_get_time()/1000);
                if (ret==ESP_OK) {
                    bool current=joke ? policy.speaking && esp_timer_get_time()/1000<=policy.expires_ms :
                                        business_action(business)!=NULL;
                    ret=current ? motion_action_execute_async(action,NULL) : ESP_ERR_INVALID_STATE;
                    if (ret==ESP_OK) {
                        active=true; expression_owned=true; active_expression=expression;
                        finish_by=esp_timer_get_time()/1000+60000;
                        renew_at=esp_timer_get_time()/1000+5000;
                    } else mipi_dsi_bridge_expression_set(previous,0);
                }
                ESP_LOGI(TAG,"Scene event=%d action=%s result=%s (accepted != completed)",
                         joke ? JOKE_REQUEST : business,action,esp_err_to_name(ret));
            } else ESP_LOGI(TAG,"Scene skipped: busy, wrong mode, stale reply or unready; no retry");
        }
        if (active) {
            motion_controller_status_t state;
            bool finished=now>=finish_by || (motion_controller_get_status(&state)==ESP_OK && state.state!=MOTION_STATE_EXECUTING);
            if (expression_owned && (finished || now>=renew_at)) {
                char current[24]; bool held=false;
                if (mipi_dsi_bridge_expression_get(current,sizeof(current),&held)==ESP_OK) {
                    if (held && !strcmp(current,active_expression))
                        mipi_dsi_bridge_expression_set(finished ? previous : active_expression,finished ? 0 : 15000);
                    else expression_owned=false; // A later manual change wins.
                }
                renew_at=esp_timer_get_time()/1000+5000;
            }
            if (finished) {
                active=false;
                ESP_LOGI(TAG,"Scene finished; expression release attempted");
            }
        }
    }
}

esp_err_t joke_scene_init(void)
{
    if (events) return ESP_OK;
    events=xQueueCreate(12,sizeof(int));
    if (!events) return ESP_ERR_NO_MEM;
    if (xTaskCreate(worker,"joke_scene",6144,NULL,3,NULL)!=pdPASS) {
        vQueueDelete(events); events=NULL; return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
