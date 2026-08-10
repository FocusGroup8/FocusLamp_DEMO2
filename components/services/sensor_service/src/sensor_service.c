/*
 * sensor_service.c - Sensor service implementation
 *
 * Radar sensor only. The ambient light sensor module (TEMT6000/ADC) has been
 * removed from the hardware, so all light sampling/smoothing logic is gone.
 */

#include "sensor_service.h"
#include "event_bus.h"
#include "event_def.h"
#include "radar_driver.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sensor_service";

static bool s_initialized = false;
static bool s_monitoring = false;

/* Registered callbacks */
static sensor_cb_t s_radar_callback = NULL;

static TaskHandle_t s_sample_task_handle = NULL;
static uint32_t s_sampling_interval_ms = 1000;

/* ===================== Event Callbacks ===================== */

static void sensor_service_event_handler(event_t *event, void *context)
{
    (void)context;

    switch (event->type) {
        case EV_SENSOR_RADAR_DETECTED:
            ESP_LOGI(TAG, "Radar detected event received");
            if (s_radar_callback != NULL) {
                s_radar_callback(SENSOR_TYPE_RADAR, event->data, event->data_size);
            }
            break;
        case EV_SENSOR_RADAR_CLEAR:
            ESP_LOGI(TAG, "Radar clear event received");
            if (s_radar_callback != NULL) {
                s_radar_callback(SENSOR_TYPE_RADAR, event->data, event->data_size);
            }
            break;
        default:
            break;
    }
}

static void sensor_service_sample_task(void *arg)
{
    while (s_monitoring) {
        /* Sample radar sensor only */
        uint8_t raw_buffer[64];
        int bytes_read = radar_driver_read_data(raw_buffer, sizeof(raw_buffer));
        if (bytes_read > 0) {
            radar_data_t radar_data;
            if (radar_driver_parse_data(raw_buffer, &radar_data) == ESP_OK) {
                event_type_t radar_evt = radar_data.target_detected
                    ? EV_SENSOR_RADAR_DETECTED
                    : EV_SENSOR_RADAR_CLEAR;

                event_t ev = {
                    .type = radar_evt,
                    .data = &radar_data,
                    .data_size = sizeof(radar_data),
                    .timestamp = event_bus_get_timestamp(),
                };
                event_bus_publish(&ev);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(s_sampling_interval_ms));
    }

    vTaskDelete(NULL);
}

/* ===================== Public API ===================== */

esp_err_t sensor_service_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing sensor service...");

    esp_err_t ret = radar_driver_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Radar driver init failed (%s), continuing without radar", esp_err_to_name(ret));
        /* Radar is optional - do not return */
    }

    /* Subscribe to sensor events */
    ret = event_bus_subscribe(EV_SENSOR_RADAR_DETECTED, sensor_service_event_handler, NULL);
    ret = event_bus_subscribe(EV_SENSOR_RADAR_CLEAR, sensor_service_event_handler, NULL);
    (void)ret;

    s_initialized = true;
    ESP_LOGI(TAG, "Sensor service initialized");
    return ESP_OK;
}

esp_err_t sensor_service_get_radar_data(struct radar_data *data)
{
    if (!s_initialized || data == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t raw_buffer[64];
    int bytes_read = radar_driver_read_data(raw_buffer, sizeof(raw_buffer));
    if (bytes_read <= 0) {
        return ESP_FAIL;
    }

    return radar_driver_parse_data(raw_buffer, (radar_data_t *)data);
}

esp_err_t sensor_service_start_monitoring(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_monitoring) {
        return ESP_OK;
    }

    s_monitoring = true;

    BaseType_t ret = xTaskCreate(
        sensor_service_sample_task,
        "sensor_monitor",
        4096,
        NULL,
        5,
        &s_sample_task_handle
    );

    if (ret != pdPASS) {
        s_monitoring = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Sensor monitoring started");
    return ESP_OK;
}

esp_err_t sensor_service_stop_monitoring(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_monitoring = false;

    if (s_sample_task_handle != NULL) {
        vTaskDelete(s_sample_task_handle);
        s_sample_task_handle = NULL;
    }

    ESP_LOGI(TAG, "Sensor monitoring stopped");
    return ESP_OK;
}

esp_err_t sensor_service_set_sampling_interval(uint32_t interval_ms)
{
    if (interval_ms < 10) {
        return ESP_ERR_INVALID_ARG;
    }

    s_sampling_interval_ms = interval_ms;
    return ESP_OK;
}

esp_err_t sensor_service_set_callback(sensor_type_t type, sensor_cb_t cb)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    switch (type) {
        case SENSOR_TYPE_RADAR:
            s_radar_callback = cb;
            break;
        default:
            return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}
