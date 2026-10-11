/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "touch_interpreter.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "touch_interpreter_config.h"
#include "touch_interpreter_types.h"
#include "touch_sensor.h"
#include "touch_sensor_types.h"

#if (TOUCH_INTERPRETER_ENABLE == 1)

static const char *TAG = "TOUCH_INTERP";

/*---------------------------------------------------------------
 * State machine states
 *
 * IDLE:          No touch activity, waiting for PRESS
 * TOUCHING:      Finger detected, waiting for RELEASE or LONG_PRESS timeout
 * WAIT_SECOND:   First TAP released, waiting for second tap or timeout
 *-------------------------------------------------------------*/
typedef enum {
    STATE_IDLE,
    STATE_TOUCHING,
    STATE_WAIT_SECOND,
} interp_state_t;

/*---------------------------------------------------------------
 * Module state
 *-------------------------------------------------------------*/
static bool s_initialized              = false;
static touch_gesture_cb_t s_gesture_cb = NULL;
static void *s_user_ctx                = NULL;

static QueueHandle_t s_event_queue      = NULL;
static TaskHandle_t s_task_handle       = NULL;
static TimerHandle_t s_double_tap_timer = NULL;

/* State machine variables */
static interp_state_t s_state     = STATE_IDLE;
static int64_t s_press_time_ms    = 0; /* Timestamp of last PRESS event */
static int64_t s_first_tap_end_ms = 0; /* Timestamp when first TAP's RELEASE occurred */
static int s_tap_count            = 0; /* Number of completed taps in current sequence */

/* Calibration helper: timestamp of the last RELEASE, to measure the gap
 * between consecutive presses. */
static int64_t s_last_release_ms = 0;

/*---------------------------------------------------------------
 * Event queue item
 *-------------------------------------------------------------*/
typedef struct {
    touch_event_t event;
    int64_t timestamp_ms;
} queue_item_t;

/*---------------------------------------------------------------
 * Forward declarations
 *-------------------------------------------------------------*/
static void touch_sensor_callback(const touch_sensor_event_data_t *event_data, void *user_ctx);
static void processing_task(void *arg);
static void double_tap_timer_callback(TimerHandle_t timer);
static void emit_gesture(touch_gesture_t gesture, int64_t timestamp_ms, int64_t duration_ms, int64_t interval_ms);
static void reset_state(void);

/*---------------------------------------------------------------
 * Touch sensor callback (ISR context)
 *
 * Enqueues raw event for deferred processing in the task context.
 *-------------------------------------------------------------*/
static void IRAM_ATTR touch_sensor_callback(const touch_sensor_event_data_t *event_data, void *user_ctx)
{
    (void)user_ctx;
    if (s_event_queue) {
        queue_item_t item = {
            .event        = event_data->event,
            .timestamp_ms = event_data->timestamp_ms,
        };
        BaseType_t higher_woken = pdFALSE;
        xQueueSendFromISR(s_event_queue, &item, &higher_woken);
        portYIELD_FROM_ISR(higher_woken);
    }
}

/*---------------------------------------------------------------
 * Emit gesture event via callback
 *-------------------------------------------------------------*/
static void emit_gesture(touch_gesture_t gesture, int64_t timestamp_ms, int64_t duration_ms, int64_t interval_ms)
{
    if (s_gesture_cb) {
        touch_gesture_event_t event = {
            .gesture      = gesture,
            .timestamp_ms = timestamp_ms,
            .duration_ms  = duration_ms,
            .interval_ms  = interval_ms,
        };
        s_gesture_cb(&event, s_user_ctx);
    }

    const char *name = "?";
    switch (gesture) {
    case TOUCH_GESTURE_TAP:
        name = "TAP";
        break;
    case TOUCH_GESTURE_DOUBLE_TAP:
        name = "DOUBLE_TAP";
        break;
    case TOUCH_GESTURE_LONG_PRESS:
        name = "LONG_PRESS";
        break;
    }
    ESP_LOGI(TAG, "Gesture: %s (duration=%lldms, interval=%lldms)", name, duration_ms, interval_ms);
}

/*---------------------------------------------------------------
 * Reset state machine to IDLE
 *-------------------------------------------------------------*/
static void reset_state(void)
{
    s_state            = STATE_IDLE;
    s_press_time_ms    = 0;
    s_first_tap_end_ms = 0;
    s_tap_count        = 0;
    if (s_double_tap_timer) {
        xTimerStop(s_double_tap_timer, 0);
    }
}

/*---------------------------------------------------------------
 * Double-tap timeout callback
 *
 * Called when the double-tap window expires without another tap.
 * Emits TAP if only one tap completed, DOUBLE_TAP if two.
 *-------------------------------------------------------------*/
static void double_tap_timer_callback(TimerHandle_t timer)
{
    (void)timer;

    if (s_state == STATE_WAIT_SECOND) {
        if (s_tap_count >= 2) {
            /* Two taps completed — emit DOUBLE_TAP */
            int64_t duration = s_first_tap_end_ms - s_press_time_ms;
            emit_gesture(TOUCH_GESTURE_DOUBLE_TAP, s_first_tap_end_ms, duration, 0);
        } else {
            /* Only one tap — emit single TAP */
            int64_t duration = s_first_tap_end_ms - s_press_time_ms;
            emit_gesture(TOUCH_GESTURE_TAP, s_first_tap_end_ms, duration, 0);
        }
        reset_state();
    }
}

/*---------------------------------------------------------------
 * Processing task
 *
 * Reads raw events from queue and runs the state machine.
 *-------------------------------------------------------------*/
static void processing_task(void *arg)
{
    (void)arg;
    queue_item_t item;

    ESP_LOGI(TAG, "Processing task started");

    while (1) {
        if (xQueueReceive(s_event_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

#if TOUCH_INTERPRETER_DEBUG_LOG
        /* Calibration helper: log raw touch timing so the user can pick
         * TAP / DOUBLE_TAP / LONG_PRESS thresholds from real measurements. */
        if (item.event == TOUCH_EVENT_PRESS) {
            if (s_last_release_ms > 0) {
                ESP_LOGI(TAG, "PRESS t=%lldms (gap since last release=%lldms)", item.timestamp_ms,
                         item.timestamp_ms - s_last_release_ms);
            } else {
                ESP_LOGI(TAG, "PRESS t=%lldms", item.timestamp_ms);
            }
        } else if (item.event == TOUCH_EVENT_RELEASE) {
            int64_t dur = item.timestamp_ms - s_press_time_ms;
            if (dur < 0) {
                dur = 0;
            }
            ESP_LOGI(TAG, "RELEASE t=%lldms (duration=%lldms)", item.timestamp_ms, dur);
            s_last_release_ms = item.timestamp_ms;
        }
#endif

        switch (s_state) {
        case STATE_IDLE:
            if (item.event == TOUCH_EVENT_PRESS) {
                s_press_time_ms = item.timestamp_ms;
                s_state         = STATE_TOUCHING;
                ESP_LOGD(TAG, "IDLE -> TOUCHING (press at %lldms)", item.timestamp_ms);
            }
            break;

        case STATE_TOUCHING: {
            if (item.event == TOUCH_EVENT_RELEASE) {
                int64_t duration = item.timestamp_ms - s_press_time_ms;

                /* Filter: touch too short = noise */
                if (duration < TOUCH_INTERPRETER_VALID_TOUCH_MIN_MS) {
                    ESP_LOGD(TAG, "TOUCHING -> IDLE (too short: %lldms)", duration);
                    reset_state();
                    break;
                }

                /* Long press already handled by timer-less check below */
                /* If touch was in TAP range */
                if (duration >= TOUCH_INTERPRETER_TAP_MIN_MS && duration <= TOUCH_INTERPRETER_TAP_MAX_MS) {
                    /* Potential TAP — wait for double-tap window */
                    s_tap_count++;
                    s_first_tap_end_ms = item.timestamp_ms;
                    s_state            = STATE_WAIT_SECOND;

                    /* Start double-tap timer */
                    if (s_double_tap_timer) {
                        xTimerStart(s_double_tap_timer, 0);
                    }
                    ESP_LOGD(TAG, "TOUCHING -> WAIT_SECOND (tap#%d, duration=%lldms)", s_tap_count, duration);
                } else {
                    /* Touch too long for TAP but not long enough for LONG_PRESS */
                    /* This is the gap between TAP_MAX and LONG_PRESS threshold */
                    ESP_LOGD(TAG, "TOUCHING -> IDLE (ambiguous duration: %lldms)", duration);
                    reset_state();
                }
            } else if (item.event == TOUCH_EVENT_PRESS) {
                /* Spurious re-press while touching — update press time */
                s_press_time_ms = item.timestamp_ms;
            }
            break;
        }

        case STATE_WAIT_SECOND: {
            if (item.event == TOUCH_EVENT_PRESS) {
                int64_t interval = item.timestamp_ms - s_first_tap_end_ms;

                /* Check if within double-tap interval */
                if (interval >= TOUCH_INTERPRETER_DOUBLE_TAP_MIN_MS &&
                    interval <= TOUCH_INTERPRETER_DOUBLE_TAP_MAX_MS) {
                    /* Valid second tap press — wait for release to confirm */
                    s_press_time_ms = item.timestamp_ms;
                    s_state         = STATE_TOUCHING;

                    /* Stop double-tap timer — we got the second press */
                    if (s_double_tap_timer) {
                        xTimerStop(s_double_tap_timer, 0);
                    }
                    ESP_LOGD(TAG, "WAIT_SECOND -> TOUCHING (second press, interval=%lldms)", interval);
                } else {
                    /* Interval too long — first tap was a single TAP */
                    int64_t first_duration = s_first_tap_end_ms - s_press_time_ms;
                    emit_gesture(TOUCH_GESTURE_TAP, s_first_tap_end_ms, first_duration, 0);

                    /* This new press starts a fresh sequence */
                    s_tap_count     = 0;
                    s_press_time_ms = item.timestamp_ms;
                    s_state         = STATE_TOUCHING;
                    if (s_double_tap_timer) {
                        xTimerStop(s_double_tap_timer, 0);
                    }
                    ESP_LOGD(TAG, "WAIT_SECOND -> TOUCHING (interval=%lldms out of range, emitted TAP)", interval);
                }
            } else if (item.event == TOUCH_EVENT_RELEASE) {
                /* Release during WAIT_SECOND without a prior PRESS — ignore */
                ESP_LOGD(TAG, "WAIT_SECOND: spurious RELEASE, ignoring");
            }
            break;
        }

        default:
            reset_state();
            break;
        }
    }
}

/*---------------------------------------------------------------
 * Long press detection helper task
 *
 * Periodically checks if touch is held beyond long press threshold.
 * Needed because we only get events on press/release edges.
 *-------------------------------------------------------------*/
static void long_press_check_task(void *arg)
{
    (void)arg;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100)); /* Check every 100ms */

        if (s_state == STATE_TOUCHING && s_press_time_ms > 0) {
            int64_t now_ms = esp_timer_get_time() / 1000;
            int64_t held   = now_ms - s_press_time_ms;
            if (held >= TOUCH_INTERPRETER_LONG_PRESS_MS) {
                emit_gesture(TOUCH_GESTURE_LONG_PRESS, now_ms, held, 0);
                reset_state();
                ESP_LOGD(TAG, "Long press detected by polling");
            }
        }
    }
}

/*---------------------------------------------------------------
 * Public API
 *-------------------------------------------------------------*/

esp_err_t touch_interpreter_init(const touch_interpreter_config_t *config)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    /* Apply config */
    int stack_size = 3072;
    int priority   = 5;
    if (config) {
        s_gesture_cb = config->gesture_cb;
        s_user_ctx   = config->user_ctx;
        if (config->task_stack_size > 0)
            stack_size = config->task_stack_size;
        if (config->task_priority > 0)
            priority = config->task_priority;
    }

    ESP_LOGI(TAG, "Initializing (tap=%d-%dms, double_tap=%d-%dms, long_press=%dms)...", TOUCH_INTERPRETER_TAP_MIN_MS,
             TOUCH_INTERPRETER_TAP_MAX_MS, TOUCH_INTERPRETER_DOUBLE_TAP_MIN_MS, TOUCH_INTERPRETER_DOUBLE_TAP_MAX_MS,
             TOUCH_INTERPRETER_LONG_PRESS_MS);

    /* Create event queue */
    s_event_queue = xQueueCreate(16, sizeof(queue_item_t));
    if (!s_event_queue) {
        ESP_LOGE(TAG, "Failed to create event queue");
        return ESP_ERR_NO_MEM;
    }

    /* Create double-tap timeout timer (one-shot) */
    s_double_tap_timer =
        xTimerCreate("dbl_tap", pdMS_TO_TICKS(TOUCH_INTERPRETER_DOUBLE_TAP_MAX_MS), pdFALSE, /* one-shot */
                     NULL, double_tap_timer_callback);
    if (!s_double_tap_timer) {
        ESP_LOGE(TAG, "Failed to create double-tap timer");
        vQueueDelete(s_event_queue);
        s_event_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Create processing task */
    BaseType_t ret = xTaskCreate(processing_task, "touch_interp", stack_size, NULL, priority, &s_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create processing task");
        xTimerDelete(s_double_tap_timer, 0);
        s_double_tap_timer = NULL;
        vQueueDelete(s_event_queue);
        s_event_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Create long-press polling task */
    ret = xTaskCreate(long_press_check_task, "touch_lpress", 2048, NULL, priority - 1, NULL);
    if (ret != pdPASS) {
        ESP_LOGW(TAG, "Failed to create long-press check task (long press detection relies on event timing only)");
    }

    /* Initialize touch_sensor with our callback */
    touch_sensor_config_t sensor_cfg = {
        .event_cb = touch_sensor_callback,
        .user_ctx = NULL,
    };
    esp_err_t err = touch_sensor_init(&sensor_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register touch sensor callback: %s", esp_err_to_name(err));
        vTaskDelete(s_task_handle);
        s_task_handle = NULL;
        xTimerDelete(s_double_tap_timer, 0);
        s_double_tap_timer = NULL;
        vQueueDelete(s_event_queue);
        s_event_queue = NULL;
        return err;
    }

    /* Reset state machine */
    reset_state();
    s_initialized = true;

    ESP_LOGI(TAG, "Touch interpreter initialized");
    return ESP_OK;
}

esp_err_t touch_interpreter_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    /* Stop and delete task */
    if (s_task_handle) {
        vTaskDelete(s_task_handle);
        s_task_handle = NULL;
    }

    /* Delete timer */
    if (s_double_tap_timer) {
        xTimerStop(s_double_tap_timer, portMAX_DELAY);
        xTimerDelete(s_double_tap_timer, 0);
        s_double_tap_timer = NULL;
    }

    /* Delete queue */
    if (s_event_queue) {
        vQueueDelete(s_event_queue);
        s_event_queue = NULL;
    }

    /* Reset state */
    reset_state();
    s_initialized = false;
    s_gesture_cb  = NULL;
    s_user_ctx    = NULL;

    ESP_LOGI(TAG, "Touch interpreter deinitialized");
    return ESP_OK;
}

bool touch_interpreter_is_initialized(void)
{
    return s_initialized;
}

#endif /* TOUCH_INTERPRETER_ENABLE == 1 */
