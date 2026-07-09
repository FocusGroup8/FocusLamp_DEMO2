/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "audio_system.h"

#include "audio/audio_manager.h"
#include "board_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "AUDIO_SYSTEM";

// Audio system state
static audio_state_t s_state             = AUDIO_STATE_IDLE;
static audio_config_t s_config           = {0};
static i2s_audio_handles_t s_i2s_handles = {0};

esp_err_t audio_system_init(const audio_config_t *config)
{
    ESP_LOGI(TAG, "Initializing audio system...");

    // Use default config if NULL
    if (config == NULL) {
        s_config.mode            = AUDIO_MODE_PLAY_ONLY;
        s_config.sample_rate     = BOARD_I2S_SAMPLE_RATE;
        s_config.record_path     = "/storage/audio/test_rec.wav";
        s_config.play_path       = "/storage/audio/music01.ogg";
        s_config.record_duration = 0;
        s_config.volume_percent  = 50;
    } else {
        s_config = *config;
    }

    // Initialize audio manager (wraps I2S, storage, recorder, player)
    esp_err_t ret = audio_manager_init(&s_i2s_handles);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize audio manager: %s", esp_err_to_name(ret));
        s_state = AUDIO_STATE_ERROR;
        return ret;
    }

    // List files in LittleFS storage (diagnostic)
    int file_count = 0;
    ret            = audio_manager_list_files(&file_count);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Files in LittleFS storage: %d", file_count);
    } else {
        ESP_LOGW(TAG, "Failed to list files in storage");
    }

    // Check if default play file exists
    ESP_LOGI(TAG, "Default play path: %s", s_config.play_path);
    FILE *test_file = fopen(s_config.play_path, "rb");
    if (test_file == NULL) {
        ESP_LOGE(TAG, "Default play file does NOT exist: %s", s_config.play_path);
        ESP_LOGW(TAG, "Please ensure the file is in littlefs_data/audio/ directory and LittleFS partition is flashed");
    } else {
        fseek(test_file, 0, SEEK_END);
        long file_size = ftell(test_file);
        fseek(test_file, 0, SEEK_SET);
        ESP_LOGI(TAG, "Default play file exists: %s (size: %ld bytes)", s_config.play_path, file_size);
        fclose(test_file);
    }

    s_state = AUDIO_STATE_IDLE;
    ESP_LOGI(TAG, "Audio system initialized successfully");
    ESP_LOGI(TAG, "Mode: %d, Sample rate: %lu Hz", s_config.mode, s_config.sample_rate);

    return ESP_OK;
}

esp_err_t audio_system_start(void)
{
    ESP_LOGI(TAG, "Starting audio system in mode %d...", s_config.mode);

    esp_err_t ret = ESP_OK;

    switch (s_config.mode) {
    case AUDIO_MODE_RECORD_ONLY:
        ret = audio_manager_start_record(s_config.record_path, s_config.record_duration);
        if (ret == ESP_OK) {
            s_state = AUDIO_STATE_RECORDING;
            ESP_LOGI(TAG, "Recording started: %s", s_config.record_path);
        }
        break;

    case AUDIO_MODE_PLAY_ONLY:
        ret = audio_manager_start_play(s_config.play_path);
        if (ret == ESP_OK) {
            s_state = AUDIO_STATE_PLAYING;
            ESP_LOGI(TAG, "Playback started: %s", s_config.play_path);
        }
        break;

    case AUDIO_MODE_LOOPBACK:
        // Loopback: real-time mic -> speaker streaming (full-duplex)
        ESP_LOGI(TAG, "Starting loopback mode (mic -> speaker)...");
        ESP_LOGE(TAG, "Loopback mode requires full-duplex I2S streaming implementation");
        ESP_LOGE(TAG, "This feature is not yet available. Please use PLAY_ONLY or RECORD_ONLY mode.");
        s_state = AUDIO_STATE_ERROR;
        ret     = ESP_ERR_NOT_SUPPORTED;
        break;

    case AUDIO_MODE_FULL:
        ESP_LOGI(TAG, "Starting full demo mode...");
        // Full demo: record + play + monitor
        s_state = AUDIO_STATE_RECORDING;
        ret     = audio_manager_start_record(s_config.record_path, s_config.record_duration);
        break;

    default:
        ESP_LOGE(TAG, "Invalid audio mode: %d", s_config.mode);
        ret = ESP_ERR_INVALID_ARG;
        break;
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start audio system: %s", esp_err_to_name(ret));
        s_state = AUDIO_STATE_ERROR;
    }

    return ret;
}

esp_err_t audio_system_stop(void)
{
    ESP_LOGI(TAG, "Stopping audio system (current state: %d)...", s_state);

    esp_err_t ret = ESP_OK;

    if (s_state == AUDIO_STATE_RECORDING || s_state == AUDIO_STATE_LOOPBACK) {
        ret = audio_manager_stop_record();
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Recording stopped");
        }
    }

    if (s_state == AUDIO_STATE_PLAYING || s_state == AUDIO_STATE_LOOPBACK) {
        ret = audio_manager_stop_play();
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Playback stopped");
        }
    }

    if (s_state == AUDIO_STATE_PAUSED) {
        ret = audio_manager_stop_play();
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Paused playback stopped");
        }
    }

    s_state = AUDIO_STATE_IDLE;
    ESP_LOGI(TAG, "Audio system stopped");

    return ret;
}

esp_err_t audio_system_pause(void)
{
    if (s_state != AUDIO_STATE_PLAYING) {
        ESP_LOGW(TAG, "Cannot pause: not playing (state: %d)", s_state);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = audio_manager_pause();
    if (ret == ESP_OK) {
        s_state = AUDIO_STATE_PAUSED;
        ESP_LOGI(TAG, "Playback paused");
    } else {
        ESP_LOGE(TAG, "Failed to pause playback: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t audio_system_resume(void)
{
    if (s_state != AUDIO_STATE_PAUSED) {
        ESP_LOGW(TAG, "Cannot resume: not paused (state: %d)", s_state);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = audio_manager_resume();
    if (ret == ESP_OK) {
        s_state = AUDIO_STATE_PLAYING;
        ESP_LOGI(TAG, "Playback resumed");
    } else {
        ESP_LOGE(TAG, "Failed to resume playback: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t audio_system_set_volume(int volume_percent)
{
    if (volume_percent < 0 || volume_percent > 100) {
        ESP_LOGE(TAG, "Invalid volume: %d (must be 0-100)", volume_percent);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = audio_manager_set_volume(volume_percent);
    if (ret == ESP_OK) {
        s_config.volume_percent = volume_percent;
        ESP_LOGI(TAG, "Volume set to %d%%", volume_percent);
    } else {
        ESP_LOGE(TAG, "Failed to set volume: %s", esp_err_to_name(ret));
    }

    return ret;
}

audio_state_t audio_system_get_state(void)
{
    return s_state;
}

void audio_system_deinit(void)
{
    ESP_LOGI(TAG, "Deinitializing audio system...");

    // Stop any active operations
    if (s_state == AUDIO_STATE_RECORDING || s_state == AUDIO_STATE_PLAYING || s_state == AUDIO_STATE_LOOPBACK ||
        s_state == AUDIO_STATE_PAUSED) {
        audio_system_stop();
    }

    // Deinitialize audio manager
    audio_manager_deinit();

    s_state = AUDIO_STATE_IDLE;
    ESP_LOGI(TAG, "Audio system deinitialized");
}