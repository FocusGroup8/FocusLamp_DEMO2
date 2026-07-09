/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "audio_manager.h"

#include "audio_storage.h"
#include "board_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "player.h"
#include "recorder.h"

static const char *TAG = "AUDIO_MANAGER";

// Audio manager state
static audio_manager_state_t s_state     = AUDIO_MANAGER_IDLE;
static i2s_audio_handles_t s_i2s_handles = {0};

esp_err_t audio_manager_init(i2s_audio_handles_t *i2s_handles)
{
    ESP_LOGI(TAG, "Initializing audio manager...");

    // Initialize I2S driver
    esp_err_t ret = i2s_audio_init(&s_i2s_handles);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S: %s", esp_err_to_name(ret));
        s_state = AUDIO_MANAGER_ERROR;
        return ret;
    }

    // Enable I2S channels
    ret = i2s_audio_enable(&s_i2s_handles);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S: %s", esp_err_to_name(ret));
        i2s_audio_deinit(&s_i2s_handles);
        s_state = AUDIO_MANAGER_ERROR;
        return ret;
    }

    // Initialize storage
    ret = audio_storage_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init storage: %s", esp_err_to_name(ret));
        i2s_audio_disable(&s_i2s_handles);
        i2s_audio_deinit(&s_i2s_handles);
        s_state = AUDIO_MANAGER_ERROR;
        return ret;
    }

    // Initialize recorder
    ret = recorder_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init recorder: %s", esp_err_to_name(ret));
        audio_storage_deinit();
        i2s_audio_disable(&s_i2s_handles);
        i2s_audio_deinit(&s_i2s_handles);
        s_state = AUDIO_MANAGER_ERROR;
        return ret;
    }

    // Initialize player
    ret = player_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init player: %s", esp_err_to_name(ret));
        recorder_deinit();
        audio_storage_deinit();
        i2s_audio_disable(&s_i2s_handles);
        i2s_audio_deinit(&s_i2s_handles);
        s_state = AUDIO_MANAGER_ERROR;
        return ret;
    }

    // Return I2S handles to caller
    if (i2s_handles) {
        *i2s_handles = s_i2s_handles;
    }

    s_state = AUDIO_MANAGER_IDLE;
    ESP_LOGI(TAG, "Audio manager initialized successfully");
    return ESP_OK;
}

void audio_manager_deinit(void)
{
    ESP_LOGI(TAG, "Deinitializing audio manager...");

    // Stop any ongoing operations
    if (s_state == AUDIO_MANAGER_RECORDING) {
        audio_manager_stop_record();
    }
    if (s_state == AUDIO_MANAGER_PLAYING || s_state == AUDIO_MANAGER_PAUSED) {
        audio_manager_stop_play();
    }

    // Deinitialize subsystems
    player_deinit();
    recorder_deinit();
    audio_storage_deinit();

    // Disable and deinit I2S
    i2s_audio_disable(&s_i2s_handles);
    i2s_audio_deinit(&s_i2s_handles);

    s_state = AUDIO_MANAGER_IDLE;
    ESP_LOGI(TAG, "Audio manager deinitialized");
}

esp_err_t audio_manager_start_record(const char *filename, uint32_t max_duration)
{
    if (s_state != AUDIO_MANAGER_IDLE) {
        ESP_LOGE(TAG, "Cannot start recording: not idle (state=%d)", s_state);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Starting recording: %s, max_duration=%u sec", filename, max_duration);

    esp_err_t ret = recorder_start(&s_i2s_handles, filename, max_duration);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start recorder: %s", esp_err_to_name(ret));
        return ret;
    }

    s_state = AUDIO_MANAGER_RECORDING;
    ESP_LOGI(TAG, "Recording started");
    return ESP_OK;
}

esp_err_t audio_manager_stop_record(void)
{
    if (s_state != AUDIO_MANAGER_RECORDING) {
        ESP_LOGW(TAG, "Cannot stop recording: not recording (state=%d)", s_state);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Stopping recording...");

    esp_err_t ret = recorder_stop();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop recorder: %s", esp_err_to_name(ret));
        s_state = AUDIO_MANAGER_ERROR;
        return ret;
    }

    // Wait for recorder to finish
    recorder_state_t rec_state = recorder_get_state();
    while (rec_state == RECORDER_STOPPING) {
        vTaskDelay(pdMS_TO_TICKS(10));
        rec_state = recorder_get_state();
    }

    s_state = AUDIO_MANAGER_IDLE;
    ESP_LOGI(TAG, "Recording stopped");
    return ESP_OK;
}

esp_err_t audio_manager_start_play(const char *filename)
{
    if (s_state != AUDIO_MANAGER_IDLE) {
        ESP_LOGE(TAG, "Cannot start playback: not idle (state=%d)", s_state);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Starting playback: %s", filename);

    esp_err_t ret = player_start(&s_i2s_handles, filename);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start player: %s", esp_err_to_name(ret));
        return ret;
    }

    s_state = AUDIO_MANAGER_PLAYING;
    ESP_LOGI(TAG, "Playback started");
    return ESP_OK;
}

esp_err_t audio_manager_pause(void)
{
    if (s_state != AUDIO_MANAGER_PLAYING) {
        ESP_LOGW(TAG, "Cannot pause: not playing (state=%d)", s_state);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Pausing playback...");

    esp_err_t ret = player_pause();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to pause player: %s", esp_err_to_name(ret));
        return ret;
    }

    s_state = AUDIO_MANAGER_PAUSED;
    ESP_LOGI(TAG, "Playback paused");
    return ESP_OK;
}

esp_err_t audio_manager_resume(void)
{
    if (s_state != AUDIO_MANAGER_PAUSED) {
        ESP_LOGW(TAG, "Cannot resume: not paused (state=%d)", s_state);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Resuming playback...");

    esp_err_t ret = player_resume();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to resume player: %s", esp_err_to_name(ret));
        return ret;
    }

    s_state = AUDIO_MANAGER_PLAYING;
    ESP_LOGI(TAG, "Playback resumed");
    return ESP_OK;
}

esp_err_t audio_manager_stop_play(void)
{
    if (s_state != AUDIO_MANAGER_PLAYING && s_state != AUDIO_MANAGER_PAUSED) {
        ESP_LOGW(TAG, "Cannot stop playback: not active (state=%d)", s_state);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Stopping playback...");

    esp_err_t ret = player_stop();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop player: %s", esp_err_to_name(ret));
        s_state = AUDIO_MANAGER_ERROR;
        return ret;
    }

    // Wait for player to finish
    player_state_t play_state = player_get_state();
    while (play_state == PLAYER_STOPPING) {
        vTaskDelay(pdMS_TO_TICKS(10));
        play_state = player_get_state();
    }

    s_state = AUDIO_MANAGER_IDLE;
    ESP_LOGI(TAG, "Playback stopped");
    return ESP_OK;
}

esp_err_t audio_manager_set_volume(int volume_percent)
{
    return player_set_volume(volume_percent);
}

audio_manager_state_t audio_manager_get_state(void)
{
    // Sync with player state to detect playback completion
    player_state_t player_state = player_get_state();

    // If player stopped naturally, update audio_manager state
    if (player_state == PLAYER_STOPPED && s_state == AUDIO_MANAGER_PLAYING) {
        ESP_LOGI(TAG, "Player stopped naturally, updating audio_manager state");
        s_state = AUDIO_MANAGER_IDLE; // Return to idle state after playback
    }

    return s_state;
}

esp_err_t audio_manager_list_files(int *count)
{
    return audio_storage_list_files(count);
}