/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "system_manager.h"

#include "audio_system.h"
#include "display_system.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "SYSTEM_MANAGER";

// System manager state
static system_state_t s_state              = SYSTEM_STATE_UNINITIALIZED;
static system_mode_t s_mode                = SYSTEM_MODE_UNDEFINED;
static system_config_t s_config            = {0};
static display_handles_t s_display_handles = {0};

esp_err_t system_manager_init(system_config_t *config)
{
    ESP_LOGI(TAG, "Initializing system manager...");

    // Determine system mode from Kconfig
#if CONFIG_EXAMPLE_SYSTEM_MODE_AUDIO
    s_mode = SYSTEM_MODE_AUDIO_ONLY;
    ESP_LOGI(TAG, "System mode: AUDIO_ONLY (from Kconfig)");
#elif CONFIG_EXAMPLE_SYSTEM_MODE_DISPLAY
    s_mode = SYSTEM_MODE_DISPLAY_ONLY;
    ESP_LOGI(TAG, "System mode: DISPLAY_ONLY (from Kconfig)");
#else
    ESP_LOGE(TAG, "No system mode selected in Kconfig!");
    s_mode  = SYSTEM_MODE_UNDEFINED;
    s_state = SYSTEM_STATE_ERROR;
    return ESP_ERR_INVALID_STATE;
#endif

    // Use provided config or defaults
    if (config != NULL) {
        s_config = *config;
    } else {
        // Initialize with default configs
        memset(&s_config, 0, sizeof(system_config_t));
        s_config.mode = s_mode;

        // Audio defaults
        s_config.audio_config.mode            = AUDIO_MODE_PLAY_ONLY;
        s_config.audio_config.sample_rate     = 16000;
        s_config.audio_config.record_path     = "/storage/audio/test_rec.wav";
        s_config.audio_config.play_path       = "/storage/audio/music01.ogg";
        s_config.audio_config.record_duration = 30;
        s_config.audio_config.volume_percent  = 50;

        // Display defaults
        s_config.display_config.mode                 = DISPLAY_MODE_TOUCH_GAME;
        s_config.display_config.enable_touch         = true;
        s_config.display_config.enable_double_buffer = true;
        s_config.display_config.enable_ppa_accel     = true;
    }

    esp_err_t ret = ESP_OK;

    // Initialize the selected subsystem
    if (s_mode == SYSTEM_MODE_AUDIO_ONLY) {
        ESP_LOGI(TAG, "Initializing audio subsystem...");
        ret = audio_system_init(&s_config.audio_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize audio system: %s", esp_err_to_name(ret));
            s_state = SYSTEM_STATE_ERROR;
            return ret;
        }
        ESP_LOGI(TAG, "Audio subsystem initialized successfully");

    } else if (s_mode == SYSTEM_MODE_DISPLAY_ONLY) {
        ESP_LOGI(TAG, "Initializing display subsystem...");
        ret = display_system_init(&s_config.display_config, &s_display_handles);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display system: %s", esp_err_to_name(ret));
            s_state = SYSTEM_STATE_ERROR;
            return ret;
        }
        ESP_LOGI(TAG, "Display subsystem initialized successfully");
    }

    s_state = SYSTEM_STATE_IDLE;
    ESP_LOGI(TAG, "System manager initialized successfully");
    return ESP_OK;
}

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

    if (s_mode == SYSTEM_MODE_AUDIO_ONLY) {
        ESP_LOGI(TAG, "Starting audio subsystem...");
        ret = audio_system_start();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start audio system: %s", esp_err_to_name(ret));
            s_state = SYSTEM_STATE_ERROR;
            return ret;
        }
        ESP_LOGI(TAG, "Audio subsystem started");

    } else if (s_mode == SYSTEM_MODE_DISPLAY_ONLY) {
        ESP_LOGI(TAG, "Starting display subsystem...");
        ret = display_system_start(&s_display_handles);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start display system: %s", esp_err_to_name(ret));
            s_state = SYSTEM_STATE_ERROR;
            return ret;
        }
        ESP_LOGI(TAG, "Display subsystem started");
    }

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

    if (s_mode == SYSTEM_MODE_AUDIO_ONLY) {
        ESP_LOGI(TAG, "Stopping audio subsystem...");
        ret = audio_system_stop();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to stop audio system: %s", esp_err_to_name(ret));
        }
        ESP_LOGI(TAG, "Audio subsystem stopped");

    } else if (s_mode == SYSTEM_MODE_DISPLAY_ONLY) {
        ESP_LOGI(TAG, "Stopping display subsystem...");
        ret = display_system_stop(&s_display_handles);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to stop display system: %s", esp_err_to_name(ret));
        }
        ESP_LOGI(TAG, "Display subsystem stopped");
    }

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
    if (s_mode == SYSTEM_MODE_AUDIO_ONLY) {
        ESP_LOGI(TAG, "Deinitializing audio subsystem...");
        audio_system_deinit();
        ESP_LOGI(TAG, "Audio subsystem deinitialized");

    } else if (s_mode == SYSTEM_MODE_DISPLAY_ONLY) {
        ESP_LOGI(TAG, "Deinitializing display subsystem...");
        display_system_deinit(&s_display_handles);
        ESP_LOGI(TAG, "Display subsystem deinitialized");
    }

    s_state = SYSTEM_STATE_UNINITIALIZED;
    s_mode  = SYSTEM_MODE_UNDEFINED;
    ESP_LOGI(TAG, "System manager deinitialized");
}

bool system_manager_is_audio_active(void)
{
    return (s_mode == SYSTEM_MODE_AUDIO_ONLY && s_state != SYSTEM_STATE_UNINITIALIZED);
}

bool system_manager_is_display_active(void)
{
    return (s_mode == SYSTEM_MODE_DISPLAY_ONLY && s_state != SYSTEM_STATE_UNINITIALIZED);
}