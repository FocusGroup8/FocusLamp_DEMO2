/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "camera_stream.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "websocket_manager.h"

#include <string.h>

static const char *TAG = "cam_stream";

#define STREAM_TASK_STACK_DEFAULT 8192
#define STREAM_TASK_PRIORITY_DEFAULT 5
#define STATS_WINDOW_FRAMES 30

typedef struct {
    camera_stream_config_t config;
    bool initialized;
    bool running;
    TaskHandle_t task_handle;
    SemaphoreHandle_t lock;

    /* Runtime state */
    int current_quality;
    int current_fps;

    /* Statistics */
    uint32_t frames_sent;
    uint32_t frames_failed;
    uint32_t frame_size_sum;
    uint32_t frame_size_count;
    int64_t last_stats_time;
    float actual_fps;
} stream_state_t;

static stream_state_t s_state = {0};

/*---------------------------------------------------------------
 * Internal: Streaming task
 *-------------------------------------------------------------*/
static void camera_stream_task(void *arg)
{
    ESP_LOGI(TAG, "Streaming task started (target_fps=%d, quality=%d)", s_state.current_fps, s_state.current_quality);

    /* frame_period is recomputed inside the loop so that runtime set_fps()
     * takes effect immediately. Previously this was a const computed once
     * at task start, which caused set_fps() to update current_fps but the
     * loop delay remained at the original value. */
    int64_t last_stats_log = esp_timer_get_time();

    while (s_state.running) {
        TickType_t tick_start   = xTaskGetTickCount();
        TickType_t frame_period = pdMS_TO_TICKS(1000 / s_state.current_fps);

        /* Capture frame */
        esp_err_t ret = camera_capture_frame(s_state.config.camera);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Frame capture failed: %s", esp_err_to_name(ret));
            s_state.frames_failed++;
            vTaskDelayUntil(&tick_start, frame_period);
            continue;
        }

        /* Encode JPEG (use current_quality so runtime set_quality() takes effect) */
        uint32_t jpeg_size = 0;
        ret                = camera_encode_jpeg(s_state.config.camera, s_state.current_quality, &jpeg_size);
        if (ret != ESP_OK || jpeg_size == 0) {
            ESP_LOGW(TAG, "JPEG encode failed: %s", esp_err_to_name(ret));
            s_state.frames_failed++;
            vTaskDelayUntil(&tick_start, frame_period);
            continue;
        }

        /* Get JPEG buffer */
        const uint8_t *jpeg_buf = camera_get_jpeg_buffer(s_state.config.camera);
        if (!jpeg_buf) {
            ESP_LOGW(TAG, "JPEG buffer NULL");
            s_state.frames_failed++;
            vTaskDelayUntil(&tick_start, frame_period);
            continue;
        }

        /* Broadcast to /camera clients */
        ret = ws_manager_server_broadcast_binary((const char *)jpeg_buf, (int)jpeg_size);
        if (ret != ESP_OK) {
            /* No clients connected is not a hard error */
            if (ret != ESP_ERR_NOT_FOUND) {
                ESP_LOGD(TAG, "Broadcast failed: %s", esp_err_to_name(ret));
            }
            s_state.frames_failed++;
        } else {
            s_state.frames_sent++;
            s_state.frame_size_sum += jpeg_size;
            s_state.frame_size_count++;
        }

        /* Periodic stats log (every 10 seconds) */
        int64_t now = esp_timer_get_time();
        if (now - last_stats_log >= 10 * 1000 * 1000) {
            uint32_t avg_size =
                (s_state.frame_size_count > 0) ? (s_state.frame_size_sum / s_state.frame_size_count) : 0;
            ESP_LOGD(TAG, "Stats: sent=%u failed=%u avg_size=%u bytes", (unsigned)s_state.frames_sent,
                     (unsigned)s_state.frames_failed, (unsigned)avg_size);
            last_stats_log = now;
        }

        /* Delay to maintain target FPS */
        vTaskDelayUntil(&tick_start, frame_period);
    }

    ESP_LOGI(TAG, "Streaming task ended");
    s_state.task_handle = NULL;
    vTaskDelete(NULL);
}

/*---------------------------------------------------------------
 * Public API
 *-------------------------------------------------------------*/
esp_err_t camera_stream_init(const camera_stream_config_t *config)
{
    if (s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!config || !config->camera) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_state, 0, sizeof(s_state));
    memcpy(&s_state.config, config, sizeof(*config));

    /* Apply defaults */
    if (s_state.config.default_quality <= 0 || s_state.config.default_quality > 100) {
        s_state.config.default_quality = 80;
    }
    if (s_state.config.default_fps <= 0 || s_state.config.default_fps > 30) {
        s_state.config.default_fps = 15;
    }
    if (s_state.config.task_stack_size <= 0) {
        s_state.config.task_stack_size = STREAM_TASK_STACK_DEFAULT;
    }
    if (s_state.config.task_priority <= 0) {
        s_state.config.task_priority = STREAM_TASK_PRIORITY_DEFAULT;
    }

    s_state.current_quality = s_state.config.default_quality;
    s_state.current_fps     = s_state.config.default_fps;

    s_state.lock = xSemaphoreCreateMutex();
    if (!s_state.lock) {
        return ESP_ERR_NO_MEM;
    }

    s_state.initialized = true;
    ESP_LOGI(TAG, "Initialized (quality=%d, fps=%d, stack=%d, prio=%d)", s_state.current_quality, s_state.current_fps,
             s_state.config.task_stack_size, s_state.config.task_priority);
    return ESP_OK;
}

void camera_stream_deinit(void)
{
    if (s_state.running) {
        camera_stream_stop();
    }
    if (s_state.lock) {
        vSemaphoreDelete(s_state.lock);
        s_state.lock = NULL;
    }
    s_state.initialized = false;
}

esp_err_t camera_stream_start(void)
{
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state.running) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Start camera pipeline */
    esp_err_t ret = camera_controller_start(s_state.config.camera);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Camera start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_state.running          = true;
    s_state.frames_sent      = 0;
    s_state.frames_failed    = 0;
    s_state.frame_size_sum   = 0;
    s_state.frame_size_count = 0;
    s_state.last_stats_time  = esp_timer_get_time();

    /* Create streaming task */
    BaseType_t xret = xTaskCreate(camera_stream_task, "cam_stream", s_state.config.task_stack_size, NULL,
                                  s_state.config.task_priority, &s_state.task_handle);
    if (xret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create streaming task");
        s_state.running = false;
        camera_controller_stop(s_state.config.camera);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Streaming started");
    return ESP_OK;
}

esp_err_t camera_stream_stop(void)
{
    if (!s_state.running) {
        return ESP_ERR_INVALID_STATE;
    }

    s_state.running = false;

    /* Wait for task to exit */
    while (s_state.task_handle != NULL) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* Stop camera pipeline */
    esp_err_t ret = camera_controller_stop(s_state.config.camera);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Camera stop failed: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "Streaming stopped (sent=%u, failed=%u)", (unsigned)s_state.frames_sent,
             (unsigned)s_state.frames_failed);
    return ESP_OK;
}

bool camera_stream_is_running(void)
{
    return s_state.running;
}

esp_err_t camera_stream_set_quality(int quality)
{
    if (quality < 1 || quality > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_state.lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_state.current_quality = quality;
    xSemaphoreGive(s_state.lock);
    ESP_LOGD(TAG, "Quality set to %d", quality);
    return ESP_OK;
}

esp_err_t camera_stream_set_fps(int fps)
{
    if (fps < 1 || fps > 30) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_state.lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_state.current_fps = fps;
    xSemaphoreGive(s_state.lock);
    ESP_LOGD(TAG, "FPS set to %d", fps);
    return ESP_OK;
}

int camera_stream_get_quality(void)
{
    return s_state.current_quality;
}

int camera_stream_get_fps(void)
{
    return s_state.current_fps;
}

void camera_stream_get_stats(uint32_t *frames_sent, uint32_t *frames_failed, uint32_t *avg_frame_size,
                             float *avg_fps_actual)
{
    if (frames_sent)
        *frames_sent = s_state.frames_sent;
    if (frames_failed)
        *frames_failed = s_state.frames_failed;
    if (avg_frame_size) {
        *avg_frame_size = (s_state.frame_size_count > 0) ? (s_state.frame_size_sum / s_state.frame_size_count) : 0;
    }
    if (avg_fps_actual) {
        int64_t elapsed_us = esp_timer_get_time() - s_state.last_stats_time;
        if (elapsed_us > 0 && s_state.frames_sent > 0) {
            *avg_fps_actual = (float)s_state.frames_sent * 1000000.0f / (float)elapsed_us;
        } else {
            *avg_fps_actual = 0.0f;
        }
    }
}
