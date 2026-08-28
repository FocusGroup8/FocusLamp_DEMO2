/*
 * sensor_service.c - Sensor service implementation
 *
 * Provides radar sensor support plus the ambient light (TEMT6000) sampling
 * pipeline: 100ms raw samples are accumulated into 2s blocks, EMA-smoothed,
 * and a smoothed value is published every 10 seconds.
 */

#include "sensor_service.h"
#include "event_bus.h"
#include "event_def.h"
#include "light_sensor_driver.h"
#include "radar_driver.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sensor_service";

static bool s_initialized = false;
static bool s_monitoring = false;

/* Registered callbacks */
static sensor_cb_t s_light_callback = NULL;
static sensor_cb_t s_radar_callback = NULL;

static TaskHandle_t s_sample_task_handle = NULL;
static uint32_t s_sampling_interval_ms = 1000;

/* ===================== PID-Style Light Smoothing =====================
 *
 * Three-stage pipeline (PID-inspired):
 *   1. Accumulate raw samples over 2-second windows (I: integration)
 *   2. Compute block average and apply EMA low-pass filter
 *   3. Publish smoothed value every 10 seconds
 *
 *   - P: proportional tracking of each 2s block average
 *   - I: integration via block averaging over 2s windows
 *   - D: damping via EMA (exponential moving average)
 */

/* --- Block accumulation ---
 * 采样周期 100ms，每 2s 构成一个块；每 5 个块（10s）发布一次。
 * 相比原实现（10s 块 + 60s 发布 + EMA 0.1），大幅加快环境光识别响应。 */
#define SAMPLES_PER_BLOCK       20   /* 100ms interval × 20 = 2s */
#define BLOCKS_PER_MINUTE        5   /* 5 × 2s = 10s publish period */

/* EMA filter coefficient (0.0 – 1.0, lower = smoother, higher = faster response) */
#define LIGHT_EMA_ALPHA        0.5f

static float   s_block_sum      = 0.0f;
static uint32_t s_block_count    = 0;
static uint32_t s_block_idx      = 0;  /* 0..BLOCKS_PER_MINUTE-1 */

/* EMA filter state */
static float   s_smoothed_lux   = 0.0f;
static bool    s_smoothed_valid = false;

/* Per-period block-average buffer (for debug/query) */
static float   s_block_ring[BLOCKS_PER_MINUTE];

uint8_t sensor_service_lux_to_level(float lux)
{
    if (lux > 1000.0f) {
        return 4;
    } else if (lux > 500.0f) {
        return 3;
    } else if (lux > 100.0f) {
        return 2;
    } else if (lux > 20.0f) {
        return 1;
    }
    return 0;
}

/* Called every time a 10-second block is complete */
static void sensor_service_on_block_complete(float block_avg)
{
    /* Store in circular buffer */
    s_block_ring[s_block_idx] = block_avg;
    s_block_idx = (s_block_idx + 1) % BLOCKS_PER_MINUTE;

    /* EMA update (first block initializes the filter) */
    if (!s_smoothed_valid) {
        s_smoothed_lux = block_avg;
        s_smoothed_valid = true;
    } else {
        s_smoothed_lux = LIGHT_EMA_ALPHA * block_avg
                       + (1.0f - LIGHT_EMA_ALPHA) * s_smoothed_lux;
    }
}

void sensor_service_feed_sample(float lux)
{
    if (!s_initialized) {
        return;
    }

    /* Accumulate into the current 10-second block */
    s_block_sum += lux;
    s_block_count++;

    if (s_block_count >= SAMPLES_PER_BLOCK) {
        float block_avg = s_block_sum / (float)SAMPLES_PER_BLOCK;
        s_block_sum = 0.0f;
        s_block_count = 0;

        sensor_service_on_block_complete(block_avg);

        /* Every BLOCKS_PER_MINUTE blocks → publish once per period */
        if (s_block_idx == 0) {
            float period_avg = 0.0f;
            for (int i = 0; i < BLOCKS_PER_MINUTE; i++) {
                period_avg += s_block_ring[i];
            }
            period_avg /= (float)BLOCKS_PER_MINUTE;

            uint8_t level = sensor_service_lux_to_level(s_smoothed_lux);
            ESP_LOGI(TAG, "Ambient light: %.0f lux (avg %.0f, level %d)  [PID-smoothed]",
                     (double)s_smoothed_lux, (double)period_avg, level);

            /* Publish smoothed sensor event */
            event_t ev = {
                .type = EV_SENSOR_AMBIENT_LIGHT_CHANGED,
                .data = &s_smoothed_lux,
                .data_size = sizeof(s_smoothed_lux),
                .timestamp = event_bus_get_timestamp(),
            };
            event_bus_publish(&ev);

            /* Notify registered callback */
            if (s_light_callback != NULL) {
                s_light_callback(SENSOR_TYPE_AMBIENT_LIGHT,
                                 &s_smoothed_lux, sizeof(s_smoothed_lux));
            }
        }
    }
}

esp_err_t sensor_service_get_smoothed_lux(float *lux)
{
    if (!s_initialized || lux == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_smoothed_valid) {
        return ESP_ERR_INVALID_STATE;
    }
    *lux = s_smoothed_lux;
    return ESP_OK;
}

/* ===================== Event Callbacks ===================== */

static void sensor_service_event_handler(event_t *event, void *context)
{
    (void)context;

    switch (event->type) {
        case EV_SENSOR_AMBIENT_LIGHT_CHANGED: {
            if (event->data != NULL && event->data_size == sizeof(float)) {
                /* Callback forwarding only — logging is done in feed_sample */
                if (s_light_callback != NULL) {
                    s_light_callback(SENSOR_TYPE_AMBIENT_LIGHT,
                                     event->data, event->data_size);
                }
            }
            break;
        }
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
        /* Sample radar sensor only — light sampling handled by sensor_task */
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

    esp_err_t ret = light_sensor_driver_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Light sensor driver init failed");
        return ret;
    }

    ret = radar_driver_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Radar driver init failed (%s), continuing without radar", esp_err_to_name(ret));
        /* Radar is optional - do not return */
    }

    /* Subscribe to sensor events */
    ret = event_bus_subscribe(EV_SENSOR_AMBIENT_LIGHT_CHANGED, sensor_service_event_handler, NULL);
    ret = event_bus_subscribe(EV_SENSOR_RADAR_DETECTED, sensor_service_event_handler, NULL);
    ret = event_bus_subscribe(EV_SENSOR_RADAR_CLEAR, sensor_service_event_handler, NULL);
    (void)ret;

    /* Initialize smoothing ring buffer */
    for (int i = 0; i < BLOCKS_PER_MINUTE; i++) {
        s_block_ring[i] = 0.0f;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Sensor service initialized");
    return ESP_OK;
}

esp_err_t sensor_service_get_light_lux(float *lux)
{
    if (!s_initialized || lux == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Use hardware oversampling (8 reads averaged) for a cleaner instant read.
     * For the fully-smoothed PID value, use sensor_service_get_smoothed_lux(). */
    return light_sensor_driver_read_averaged(lux, 8);
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
        case SENSOR_TYPE_AMBIENT_LIGHT:
            s_light_callback = cb;
            break;
        case SENSOR_TYPE_RADAR:
            s_radar_callback = cb;
            break;
        default:
            return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}
