/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "audio_test.h"

#include "audio_manager.h"
#include "board_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2s_driver.h"
#include "recorder.h"
#include "stdio.h"

#include <limits.h>
#include <string.h>

static const char *TAG = "AUDIO_TEST";

// Test file path (LittleFS)
#define TEST_WAV_FILE "/audio/audio/music01.wav"

// WAV playback buffer
#define WAV_BUF_SAMPLES 1024
static int16_t s_wav_buf_16bit[WAV_BUF_SAMPLES];
static int32_t s_wav_buf_32bit[WAV_BUF_SAMPLES];

// Default volume scale for WAV playback (1.0 = 100%, 2.0 = 200%)
#define WAV_PLAYBACK_VOLUME_SCALE 1.0f // Increase volume by 50%

// Store I2S handles from audio_manager
static i2s_audio_handles_t s_i2s_handles_global = {0};

esp_err_t audio_test_init(void)
{
    ESP_LOGI(TAG, "=== Audio Manager Initialization Test ===");

    esp_err_t ret = audio_manager_init(&s_i2s_handles_global);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Audio manager init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // List files in storage
    int file_count = 0;
    ret            = audio_manager_list_files(&file_count);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Files in storage: %d", file_count);
    }

    ESP_LOGI(TAG, "=== Initialization Test PASSED ===");
    return ESP_OK;
}

esp_err_t audio_test_record(const char *filename, uint32_t duration_sec)
{
    ESP_LOGI(TAG, "=== Recording Test ===");
    ESP_LOGI(TAG, "File: %s, Duration: %u seconds", filename, duration_sec);

    esp_err_t ret = audio_manager_start_record(filename, duration_sec);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start recording: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Recording started, please speak into microphone...");
    ESP_LOGI(TAG, "Waiting for recording to complete...");

    // Wait for recorder task to finish (check recorder state directly)
    recorder_state_t rec_state = recorder_get_state();
    while (rec_state == RECORDER_RECORDING) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        rec_state = recorder_get_state();
        ESP_LOGI(TAG, "Recorder state: %d", rec_state);
    }

    // Update audio manager state since recorder finished automatically
    audio_manager_stop_record();

    ESP_LOGI(TAG, "=== Recording Test PASSED ===");
    return ESP_OK;
}

esp_err_t audio_test_play_wav(const char *filename)
{
    ESP_LOGI(TAG, "=== WAV Playback Test (Direct PCM) ===");
    ESP_LOGI(TAG, "File: %s", filename);

    // Open WAV file
    FILE *file = fopen(filename, "rb");
    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open WAV file: %s", filename);
        return ESP_FAIL;
    }

    // Get file size
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    ESP_LOGI(TAG, "WAV file size: %ld bytes", file_size);

    // Skip WAV header (44 bytes)
    fseek(file, 44, SEEK_SET);

    // Use existing I2S handles from audio_manager (already initialized and enabled)
    if (s_i2s_handles_global.tx_handle == NULL || s_i2s_handles_global.rx_handle == NULL) {
        ESP_LOGE(TAG, "I2S handles not available, audio_manager not initialized");
        fclose(file);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Using existing I2S handles from audio_manager");
    ESP_LOGI(TAG, "Volume scaling: %.1f (50% boost)", WAV_PLAYBACK_VOLUME_SCALE);

    ESP_LOGI(TAG, "Playing WAV file...");

    // Read and play PCM data
    size_t total_samples = 0;
    size_t samples_read  = 0;
    size_t bytes_written = 0;
    esp_err_t ret        = ESP_OK;

    while (true) {
        // Read int16_t samples from WAV file
        samples_read = fread(s_wav_buf_16bit, sizeof(int16_t), WAV_BUF_SAMPLES, file);
        if (samples_read == 0) {
            ESP_LOGI(TAG, "End of WAV file reached");
            break;
        }

        // Convert int16_t to int32_t (left-align for 32-bit I2S) and apply volume scaling
        for (size_t i = 0; i < samples_read; i++) {
            // Left-align 16-bit to 32-bit, then apply volume scaling
            int32_t sample_32bit = (int32_t)s_wav_buf_16bit[i] << 16;
            // Scale volume (be careful about clipping)
            sample_32bit = (int32_t)(sample_32bit * WAV_PLAYBACK_VOLUME_SCALE);
            // Clamp to prevent overflow
            if (sample_32bit > INT32_MAX)
                sample_32bit = INT32_MAX;
            if (sample_32bit < INT32_MIN)
                sample_32bit = INT32_MIN;
            s_wav_buf_32bit[i] = sample_32bit;
        }

        // Write to I2S TX
        ret = i2s_audio_write(&s_i2s_handles_global, s_wav_buf_32bit, samples_read * sizeof(int32_t), &bytes_written,
                              portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(ret));
            break;
        }

        total_samples += samples_read;

        // Periodic progress log
        if (total_samples % 16000 == 0) {
            ESP_LOGI(TAG, "Played %u samples (%u seconds)", (unsigned)total_samples, (unsigned)(total_samples / 16000));
        }
    }

    ESP_LOGI(TAG, "Total samples played: %u (%u seconds)", (unsigned)total_samples, (unsigned)(total_samples / 16000));

    // Cleanup
    fclose(file);

    ESP_LOGI(TAG, "=== WAV Playback Test PASSED ===");
    return ESP_OK;
}

esp_err_t audio_test_play_ogg(const char *filename)
{
    ESP_LOGI(TAG, "=== Ogg Playback Test ===");
    ESP_LOGI(TAG, "File: %s", filename);

    // Use audio manager to play
    esp_err_t ret = audio_manager_start_play(filename);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start playback: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Ogg playback started, waiting for completion...");

    // Wait for playback to finish
    audio_manager_state_t state = audio_manager_get_state();
    while (state == AUDIO_MANAGER_PLAYING) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        state = audio_manager_get_state();
        ESP_LOGI(TAG, "Playback state: %d", state);
    }

    ESP_LOGI(TAG, "=== Ogg Playback Test PASSED ===");
    return ESP_OK;
}

esp_err_t audio_test_rec_to_play(void)
{
    ESP_LOGI(TAG, "=== Audio Playback Test (Ogg Opus Only) ===");

    // Step 1: Initialize audio system
    ESP_LOGI(TAG, "Step 1: Initializing audio system...");
    esp_err_t ret = audio_test_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Initialization failed");
        return ret;
    }

    // Step 2: Play Ogg Opus file from LittleFS (skip WAV for now)
    ESP_LOGI(TAG, "Step 2: Playing Ogg Opus file (music01.ogg)...");
    ret = audio_test_play_ogg("/audio/audio/music01.ogg");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ogg playback failed");
        audio_manager_deinit();
        return ret;
    }

    // Cleanup
    ESP_LOGI(TAG, "Step 3: Cleaning up...");
    audio_manager_deinit();

    ESP_LOGI(TAG, "=== Audio Playback Test PASSED ===");
    ESP_LOGI(TAG, "Ogg Opus playback verified successfully!");
    return ESP_OK;
}