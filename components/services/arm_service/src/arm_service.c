/*
 * arm_service.c - Mechanical arm service implementation
 */

#include "arm_service.h"
#include "event_bus.h"
#include "event_def.h"
#include "servo_service.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "arm_service";

static bool s_initialized = false;

/* Current action sequence */
static const action_sequence_t *s_current_seq = NULL;
static uint16_t s_current_step_index = 0;
static uint16_t s_current_loop = 0;
static float s_speed_multiplier = 1.0f;

static arm_status_t s_status = ARM_STATUS_IDLE;

static TaskHandle_t s_sequence_task = NULL;

/* ===================== Event Callbacks ===================== */

static void arm_service_event_handler(event_t *event, void *context)
{
    (void)context;
    if (event == NULL) {
        return;
    }
    switch (event->type) {
        case EV_ARM_SEQUENCE_START:
            arm_service_start_action();
            break;
        case EV_ARM_SEQUENCE_STOP:
            arm_service_stop_action();
            break;
        case EV_ARM_SEQUENCE_PAUSE:
            if (s_status == ARM_STATUS_RUNNING) {
                s_status = ARM_STATUS_PAUSED;
            }
            break;
        case EV_ARM_EMERGENCY_STOP:
            arm_service_stop_action();
            break;
        case EV_ARM_POSITION_REACHED:
            /* Position reached notification; sequence task handles advancement. */
            break;
        case EV_ARM_SEQUENCE_DONE:
            ESP_LOGI(TAG, "Sequence completed");
            s_status = ARM_STATUS_IDLE;
            break;
        case EV_ARM_ERROR:
            ESP_LOGE(TAG, "Arm error event received");
            s_status = ARM_STATUS_ERROR;
            break;
        default:
            break;
    }
}

/* ===================== Internal ===================== */

static void arm_service_publish_event(event_type_t type);

static void arm_service_execute_step(const action_step_t *step)
{
    if (!step) {
        return;
    }

    /* Apply speed multiplier to duration */
    uint32_t adjusted_duration = (uint32_t)(step->duration_ms / s_speed_multiplier);
    if (adjusted_duration < 10) {
        adjusted_duration = 10; /* Minimum 10ms */
    }

    ESP_LOGD(TAG, "Step: servo=%d pos=%u dur=%u delay=%u",
             step->servo_id, step->position, adjusted_duration, step->delay_ms);

    /* Set servo position (non-blocking; task waits for duration + delay) */
    servo_service_set_position(step->servo_id, step->position);
}

static void arm_service_sequence_task(void *arg)
{
    (void)arg;

    while (s_status == ARM_STATUS_RUNNING || s_status == ARM_STATUS_PAUSED) {
        if (s_current_seq == NULL) {
            break;
        }

        if (s_status == ARM_STATUS_PAUSED) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (s_current_step_index >= s_current_seq->step_count) {
            if (s_current_seq->loop_count == 0 || ++s_current_loop < s_current_seq->loop_count) {
                s_current_step_index = 0;
            } else {
                break;
            }
        }

        const action_step_t *step = &s_current_seq->steps[s_current_step_index];
        arm_service_execute_step(step);

        uint32_t wait_ms = (uint32_t)(step->duration_ms / s_speed_multiplier) + step->delay_ms;
        if (wait_ms < 10) {
            wait_ms = 10;
        }
        vTaskDelay(pdMS_TO_TICKS(wait_ms));

        s_current_step_index++;
    }

    s_status = ARM_STATUS_IDLE;
    s_sequence_task = NULL;
    arm_service_publish_event(EV_ARM_SEQUENCE_DONE);
    vTaskDelete(NULL);
}

static void arm_service_publish_event(event_type_t type)
{
    event_t ev = {
        .type = type,
        .data = NULL,
        .data_size = 0,
        .timestamp = event_bus_get_timestamp(),
    };
    event_bus_publish(&ev);
}

/* ===================== Public API ===================== */

esp_err_t arm_service_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing arm service...");

    /* Note: servo_service should be initialized by the app layer before arm_service */
    /* arm_service uses servo_service API directly */

    /* Subscribe to arm events */
    event_bus_subscribe(EV_ARM_SEQUENCE_START, arm_service_event_handler, NULL);
    event_bus_subscribe(EV_ARM_SEQUENCE_STOP, arm_service_event_handler, NULL);
    event_bus_subscribe(EV_ARM_SEQUENCE_PAUSE, arm_service_event_handler, NULL);
    event_bus_subscribe(EV_ARM_EMERGENCY_STOP, arm_service_event_handler, NULL);
    event_bus_subscribe(EV_ARM_POSITION_REACHED, arm_service_event_handler, NULL);
    event_bus_subscribe(EV_ARM_SEQUENCE_DONE, arm_service_event_handler, NULL);
    event_bus_subscribe(EV_ARM_ERROR, arm_service_event_handler, NULL);

    s_initialized = true;
    ESP_LOGI(TAG, "Arm service initialized");
    return ESP_OK;
}

esp_err_t arm_service_load_action(const action_sequence_t *seq)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (seq == NULL || seq->steps == NULL || seq->step_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Cannot load while running */
    if (s_status == ARM_STATUS_RUNNING) {
        return ESP_ERR_INVALID_STATE;
    }

    s_current_seq = seq;
    s_current_step_index = 0;
    s_current_loop = 0;

    ESP_LOGI(TAG, "Action sequence loaded: %u steps, %u loops",
             seq->step_count, seq->loop_count);

    return ESP_OK;
}

esp_err_t arm_service_start_action(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_current_seq == NULL) {
        ESP_LOGW(TAG, "No action sequence loaded");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_status == ARM_STATUS_RUNNING) {
        return ESP_OK;
    }

    s_status = ARM_STATUS_RUNNING;
    s_current_step_index = 0;
    s_current_loop = 0;

    ESP_LOGI(TAG, "Starting action sequence");

    if (s_sequence_task != NULL) {
        vTaskDelete(s_sequence_task);
        s_sequence_task = NULL;
    }

    BaseType_t ret = xTaskCreate(
        arm_service_sequence_task,
        "arm_seq",
        4096,
        NULL,
        5,
        &s_sequence_task
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create arm sequence task");
        s_status = ARM_STATUS_IDLE;
        return ESP_FAIL;
    }

    arm_service_publish_event(EV_ARM_SEQUENCE_START);

    return ESP_OK;
}

esp_err_t arm_service_stop_action(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_status == ARM_STATUS_IDLE) {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Emergency stop - halting arm action");

    s_status = ARM_STATUS_IDLE;

    if (s_sequence_task != NULL) {
        vTaskDelete(s_sequence_task);
        s_sequence_task = NULL;
    }

    /* Disable servos for safety */
    servo_service_disable();

    /* Publish emergency stop and sequence done events */
    arm_service_publish_event(EV_ARM_EMERGENCY_STOP);
    arm_service_publish_event(EV_ARM_SEQUENCE_DONE);

    return ESP_OK;
}

esp_err_t arm_service_set_speed_multiplier(float multiplier)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (multiplier < 0.1f || multiplier > 10.0f) {
        return ESP_ERR_INVALID_ARG;
    }

    s_speed_multiplier = multiplier;
    ESP_LOGI(TAG, "Speed multiplier set to %.1f", multiplier);

    return ESP_OK;
}

arm_status_t arm_service_get_status(void)
{
    return s_status;
}