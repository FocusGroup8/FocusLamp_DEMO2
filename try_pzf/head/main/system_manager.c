/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "system_manager.h"

#if CONFIG_EXAMPLE_ENABLE_AUDIO
#include "audio_system.h"
#endif

#if CONFIG_EXAMPLE_ENABLE_DISPLAY
#include "display_system.h"
#endif

#if CONFIG_EXAMPLE_ENABLE_CAMERA
#include "board_config.h"
#include "camera_controller.h"
#include "camera_preview.h"
#endif

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

#if CONFIG_EXAMPLE_ENABLE_CAMERA && CONFIG_EXAMPLE_ENABLE_DISPLAY
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "lvgl_display.h"
#endif

static const char *TAG = "SYSTEM_MANAGER";

// System manager state
static system_state_t s_state   = SYSTEM_STATE_UNINITIALIZED;
static system_mode_t s_mode     = SYSTEM_MODE_UNDEFINED;
static system_config_t s_config = {0};

#if CONFIG_EXAMPLE_ENABLE_DISPLAY
static display_handles_t s_display_handles = {0};
#endif

#if CONFIG_EXAMPLE_ENABLE_CAMERA
static camera_handles_t s_camera_handles = {0};
static camera_preview_t s_camera_preview = {0};
static TaskHandle_t s_preview_task       = NULL;
static volatile bool s_preview_running   = false;
#endif

esp_err_t system_manager_init(system_config_t *config)
{
    ESP_LOGI(TAG, "Initializing system manager...");

    // Determine system mode from Kconfig
#if CONFIG_EXAMPLE_BOARD_TYPE_BOTTOM
    s_mode = SYSTEM_MODE_BOTTOM_BOARD;
    ESP_LOGI(TAG, "Board type: BOTTOM (Audio) (from Kconfig)");
#elif CONFIG_EXAMPLE_BOARD_TYPE_TOP
    s_mode = SYSTEM_MODE_TOP_BOARD;
    ESP_LOGI(TAG, "Board type: TOP (Display/LED/Camera) (from Kconfig)");
#else
    ESP_LOGE(TAG, "No board type selected in Kconfig!");
    s_mode  = SYSTEM_MODE_UNDEFINED;
    s_state = SYSTEM_STATE_ERROR;
    return ESP_ERR_INVALID_STATE;
#endif

    // Use provided config or defaults
    if (config != NULL) {
        s_config = *config;
    } else {
        memset(&s_config, 0, sizeof(system_config_t));
        s_config.mode = s_mode;

#if CONFIG_EXAMPLE_ENABLE_AUDIO
        s_config.audio_config.mode            = AUDIO_MODE_PLAY_ONLY;
        s_config.audio_config.sample_rate     = 16000;
        s_config.audio_config.record_path     = "/storage/audio/test_rec.wav";
        s_config.audio_config.play_path       = "/storage/audio/music01.ogg";
        s_config.audio_config.record_duration = 30;
        s_config.audio_config.volume_percent  = 50;
#endif

#if CONFIG_EXAMPLE_ENABLE_DISPLAY
        s_config.display_config.mode                 = display_mode_from_kconfig();
        s_config.display_config.enable_touch         = true;
        s_config.display_config.enable_double_buffer = true;
        s_config.display_config.enable_ppa_accel     = true;
#endif

#if CONFIG_EXAMPLE_ENABLE_CAMERA
        s_config.camera_config.format_name = BOARD_CAM_FORMAT_NAME;
        s_config.camera_config.h_res       = BOARD_CAM_H_RES;
        s_config.camera_config.v_res       = BOARD_CAM_V_RES;
#endif
    }

    esp_err_t ret = ESP_OK;

    // Initialize the selected subsystem
#if CONFIG_EXAMPLE_ENABLE_AUDIO
    if (s_mode == SYSTEM_MODE_BOTTOM_BOARD) {
        ESP_LOGI(TAG, "Initializing audio subsystem...");
        ret = audio_system_init(&s_config.audio_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize audio system: %s", esp_err_to_name(ret));
            s_state = SYSTEM_STATE_ERROR;
            return ret;
        }
        ESP_LOGI(TAG, "Audio subsystem initialized successfully");
    }
#endif

#if CONFIG_EXAMPLE_ENABLE_DISPLAY
    if (s_mode == SYSTEM_MODE_TOP_BOARD) {
        ESP_LOGI(TAG, "Initializing display subsystem...");
        ret = display_system_init(&s_config.display_config, &s_display_handles);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display system: %s", esp_err_to_name(ret));
            s_state = SYSTEM_STATE_ERROR;
            return ret;
        }
        ESP_LOGI(TAG, "Display subsystem initialized successfully");
    }
#endif

#if CONFIG_EXAMPLE_ENABLE_CAMERA
    if (s_mode == SYSTEM_MODE_TOP_BOARD) {
        ESP_LOGI(TAG, "Initializing camera subsystem...");
        ret = camera_controller_init(&s_config.camera_config, &s_camera_handles);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize camera system: %s", esp_err_to_name(ret));
            s_state = SYSTEM_STATE_ERROR;
            return ret;
        }
        ESP_LOGI(TAG, "Camera subsystem initialized successfully");

        /* Initialize camera preview (PPA scaling) - only when local screen echo is enabled */
#if CONFIG_EXAMPLE_ENABLE_CAMERA_PREVIEW
        ret = camera_preview_init(&s_camera_preview);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize camera preview: %s", esp_err_to_name(ret));
            s_state = SYSTEM_STATE_ERROR;
            return ret;
        }
        ESP_LOGI(TAG, "Camera preview initialized");
#endif
    }
#endif

    s_state = SYSTEM_STATE_IDLE;
    ESP_LOGI(TAG, "System manager initialized successfully");
    return ESP_OK;
}

#if CONFIG_EXAMPLE_ENABLE_CAMERA && CONFIG_EXAMPLE_ENABLE_DISPLAY
/**
 * @brief Camera preview task
 *
 * Continuously captures camera frames, processes through PPA,
 * and updates the LVGL canvas for real-time display.
 */
#if CONFIG_EXAMPLE_ENABLE_CAMERA_PREVIEW
static void camera_preview_task(void *arg)
{
    ESP_LOGI(TAG, "Camera preview task started");

    /* Get the LVGL context from display handles */
    lvgl_display_t *lvgl_ctx = (lvgl_display_t *)s_display_handles.lvgl_ctx;
    if (lvgl_ctx == NULL) {
        ESP_LOGE(TAG, "No LVGL context available for preview");
        s_preview_running = false;
        vTaskDelete(NULL);
        return;
    }

    /* Create LVGL canvas with PPA output buffer (RGB565 little-endian format) */
    uint8_t *preview_buf = camera_preview_get_buffer(&s_camera_preview);
    if (preview_buf == NULL) {
        ESP_LOGE(TAG, "No preview buffer available");
        s_preview_running = false;
        vTaskDelete(NULL);
        return;
    }

    esp_err_t ret = lvgl_display_create_canvas(lvgl_ctx, preview_buf, 400, 320);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create camera preview canvas");
        s_preview_running = false;
        vTaskDelete(NULL);
        return;
    }

    uint32_t frame_count = 0;
    while (s_preview_running) {
        /* Capture one frame (outside LVGL lock — I/O-bound, no display access) */
        ret = camera_capture_frame(&s_camera_handles);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Frame capture failed, retrying...");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* Process through PPA OUTSIDE LVGL lock (official ESP-BSP pattern).
         * PPA is hardware-accelerated and writes to a separate buffer,
         * so it doesn't conflict with LVGL rendering the previous frame.
         */
        ret = camera_preview_process_frame(&s_camera_handles, &s_camera_preview);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "PPA process failed, skipping frame");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Lock LVGL only for canvas update (short critical section) */
        lvgl_port_lock(0);
        lvgl_display_update_canvas(lvgl_ctx, preview_buf, 400, 320);
        lvgl_port_unlock();

        frame_count++;
        if (frame_count % 30 == 0) {
            ESP_LOGD(TAG, "Preview: %lu frames processed", (unsigned long)frame_count);
        }
    }

    ESP_LOGI(TAG, "Camera preview task stopped (%lu frames total)", (unsigned long)frame_count);
    vTaskDelete(NULL);
}
#endif /* CONFIG_EXAMPLE_ENABLE_CAMERA_PREVIEW */
#endif /* CONFIG_EXAMPLE_ENABLE_CAMERA && CONFIG_EXAMPLE_ENABLE_DISPLAY */

esp_err_t system_manager_start(void)
{
    ESP_LOGI(TAG, "Starting system manager...");

    if (s_state == SYSTEM_STATE_UNINITIALIZED) {
        ESP_LOGE(TAG, "System manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_state == SYSTEM_STATE_RUNNING) {
        ESP_LOGW(TAG, "System already running");
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;

#if CONFIG_EXAMPLE_ENABLE_AUDIO
    if (s_mode == SYSTEM_MODE_BOTTOM_BOARD) {
        ESP_LOGI(TAG, "Starting audio subsystem...");
        ret = audio_system_start();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start audio system: %s", esp_err_to_name(ret));
            s_state = SYSTEM_STATE_ERROR;
            return ret;
        }
        ESP_LOGI(TAG, "Audio subsystem started");
    }
#endif

#if CONFIG_EXAMPLE_ENABLE_DISPLAY
    if (s_mode == SYSTEM_MODE_TOP_BOARD) {
        ESP_LOGI(TAG, "Starting display subsystem...");
        ret = display_system_start(&s_display_handles);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start display system: %s", esp_err_to_name(ret));
            s_state = SYSTEM_STATE_ERROR;
            return ret;
        }
        ESP_LOGI(TAG, "Display subsystem started");
    }
#endif

#if CONFIG_EXAMPLE_ENABLE_CAMERA
    if (s_mode == SYSTEM_MODE_TOP_BOARD) {
        ESP_LOGI(TAG, "Starting camera subsystem...");
        ret = camera_controller_start(&s_camera_handles);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start camera system: %s", esp_err_to_name(ret));
            s_state = SYSTEM_STATE_ERROR;
            return ret;
        }
        ESP_LOGI(TAG, "Camera subsystem started");

        /* Start camera preview task (only when local screen echo is enabled) */
#if CONFIG_EXAMPLE_ENABLE_CAMERA_PREVIEW
        if (s_config.display_config.mode == DISPLAY_MODE_LVGL) {
            ESP_LOGI(TAG, "Starting camera preview task...");
            s_preview_running = true;
            BaseType_t xret =
                xTaskCreatePinnedToCore(camera_preview_task, "cam_preview", 8192, NULL, 3, &s_preview_task, 1);
            if (xret != pdPASS) {
                ESP_LOGE(TAG, "Failed to create camera preview task");
                s_preview_running = false;
            } else {
                ESP_LOGI(TAG, "Camera preview task created");
            }
        }
#endif
    }
#endif

    s_state = SYSTEM_STATE_RUNNING;
    ESP_LOGI(TAG, "System manager started successfully");
    return ESP_OK;
}

esp_err_t system_manager_stop(void)
{
    ESP_LOGI(TAG, "Stopping system manager...");

    if (s_state != SYSTEM_STATE_RUNNING) {
        ESP_LOGW(TAG, "System not running (state: %d)", s_state);
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;

#if CONFIG_EXAMPLE_ENABLE_CAMERA && CONFIG_EXAMPLE_ENABLE_DISPLAY
    /* Stop camera preview task first */
    if (s_preview_running) {
        ESP_LOGI(TAG, "Stopping camera preview task...");
        s_preview_running = false;
        if (s_preview_task != NULL) {
            vTaskDelay(pdMS_TO_TICKS(200)); /* Wait for task to finish */
            s_preview_task = NULL;
        }
        ESP_LOGI(TAG, "Camera preview task stopped");
    }
#endif

#if CONFIG_EXAMPLE_ENABLE_AUDIO
    if (s_mode == SYSTEM_MODE_BOTTOM_BOARD) {
        ESP_LOGI(TAG, "Stopping audio subsystem...");
        ret = audio_system_stop();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to stop audio system: %s", esp_err_to_name(ret));
        }
        ESP_LOGI(TAG, "Audio subsystem stopped");
    }
#endif

#if CONFIG_EXAMPLE_ENABLE_DISPLAY
    if (s_mode == SYSTEM_MODE_TOP_BOARD) {
        ESP_LOGI(TAG, "Stopping display subsystem...");
        ret = display_system_stop(&s_display_handles);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to stop display system: %s", esp_err_to_name(ret));
        }
        ESP_LOGI(TAG, "Display subsystem stopped");
    }
#endif

#if CONFIG_EXAMPLE_ENABLE_CAMERA
    if (s_mode == SYSTEM_MODE_TOP_BOARD) {
        ESP_LOGI(TAG, "Stopping camera subsystem...");
        ret = camera_controller_stop(&s_camera_handles);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to stop camera system: %s", esp_err_to_name(ret));
        }
        ESP_LOGI(TAG, "Camera subsystem stopped");
    }
#endif

    s_state = SYSTEM_STATE_IDLE;
    ESP_LOGI(TAG, "System manager stopped");
    return ret;
}

system_mode_t system_manager_get_mode(void)
{
    return s_mode;
}

system_state_t system_manager_get_state(void)
{
    return s_state;
}

void system_manager_deinit(void)
{
    ESP_LOGI(TAG, "Deinitializing system manager...");

    // Stop if running
    if (s_state == SYSTEM_STATE_RUNNING) {
        system_manager_stop();
    }

    // Deinitialize subsystem
#if CONFIG_EXAMPLE_ENABLE_AUDIO
    if (s_mode == SYSTEM_MODE_BOTTOM_BOARD) {
        ESP_LOGI(TAG, "Deinitializing audio subsystem...");
        audio_system_deinit();
        ESP_LOGI(TAG, "Audio subsystem deinitialized");
    }
#endif

#if CONFIG_EXAMPLE_ENABLE_DISPLAY
    if (s_mode == SYSTEM_MODE_TOP_BOARD) {
        ESP_LOGI(TAG, "Deinitializing display subsystem...");
        display_system_deinit(&s_display_handles);
        ESP_LOGI(TAG, "Display subsystem deinitialized");
    }
#endif

#if CONFIG_EXAMPLE_ENABLE_CAMERA
    if (s_mode == SYSTEM_MODE_TOP_BOARD) {
        ESP_LOGI(TAG, "Deinitializing camera subsystem...");
#if CONFIG_EXAMPLE_ENABLE_CAMERA_PREVIEW
        camera_preview_deinit(&s_camera_preview);
#endif
        camera_controller_deinit(&s_camera_handles);
        ESP_LOGI(TAG, "Camera subsystem deinitialized");
    }
#endif

    s_state = SYSTEM_STATE_UNINITIALIZED;
    s_mode  = SYSTEM_MODE_UNDEFINED;
    ESP_LOGI(TAG, "System manager deinitialized");
}

bool system_manager_is_audio_active(void)
{
#if CONFIG_EXAMPLE_ENABLE_AUDIO
    return (s_mode == SYSTEM_MODE_BOTTOM_BOARD && s_state != SYSTEM_STATE_UNINITIALIZED);
#else
    return false;
#endif
}

bool system_manager_is_display_active(void)
{
#if CONFIG_EXAMPLE_ENABLE_DISPLAY
    return (s_mode == SYSTEM_MODE_TOP_BOARD && s_state != SYSTEM_STATE_UNINITIALIZED);
#else
    return false;
#endif
}

#if CONFIG_EXAMPLE_ENABLE_CAMERA
camera_handles_t *system_manager_get_camera_handles(void)
{
    if (s_mode != SYSTEM_MODE_TOP_BOARD || s_state == SYSTEM_STATE_UNINITIALIZED) {
        return NULL;
    }
    return &s_camera_handles;
}
#endif
