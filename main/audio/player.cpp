/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "player.h"
#include "board_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "stdio.h"
#include <string.h>

// Use native micro-opus OGG Opus decoder (stable, no crashes)
#include <micro_opus/ogg_opus_decoder.h>

static const char *TAG = "PLAYER";

// Player state
static player_state_t s_state = PLAYER_IDLE;
static TaskHandle_t s_task_handle = NULL;
static FILE *s_file = NULL;
static const i2s_audio_handles_t *s_i2s_handles = NULL;

// Volume control (0.0 - 1.0)
static float s_volume_scale = 1.0f;

// Decoder input buffer (OGG file data)
#define PLAYER_INPUT_BUF_SIZE 4096
static uint8_t s_input_buf[PLAYER_INPUT_BUF_SIZE];

// Decoder output buffer (PCM data)
#define PLAYER_OUTPUT_BUF_SIZE 8192
static int16_t s_output_pcm[PLAYER_OUTPUT_BUF_SIZE / sizeof(int16_t)];

// Task stack (native Opus is stable, only needs 8KB)
#define PLAYER_TASK_STACK_SIZE 8192

// Resample buffer (48kHz -> 16kHz, ratio 3:1)
#define RESAMPLE_RATIO 3
static int16_t s_resample_buf[PLAYER_OUTPUT_BUF_SIZE / sizeof(int16_t) / RESAMPLE_RATIO];

/**
 * @brief Software resample: 48kHz -> 16kHz (simple downsampling)
 *
 * @param input      Input PCM samples at 48kHz
 * @param input_samples Number of input samples
 * @param output     Output buffer for 16kHz samples
 * @param output_samples Number of output samples
 */
static void resample_48k_to_16k(const int16_t *input, size_t input_samples,
                                 int16_t *output, size_t *output_samples)
{
    // Simple downsampling: take every 3rd sample
    size_t out_idx = 0;
    for (size_t i = 0; i < input_samples; i += RESAMPLE_RATIO) {
        output[out_idx++] = input[i];
    }
    *output_samples = out_idx;
}

/**
 * @brief Scale PCM samples by volume factor
 *
 * @param samples  PCM sample buffer (32-bit signed)
 * @param count    Number of samples
 * @param scale    Volume scale factor (0.0 - 1.0)
 */
static void scale_pcm_samples(int32_t *samples, size_t count, float scale)
{
    if (scale == 1.0f) {
        return;  // No scaling needed
    }

    for (size_t i = 0; i < count; i++) {
        int64_t scaled = (int64_t)samples[i] * scale;
        // Clamp to 32-bit range
        if (scaled > INT32_MAX) {
            samples[i] = INT32_MAX;
        } else if (scaled < INT32_MIN) {
            samples[i] = INT32_MIN;
        } else {
            samples[i] = (int32_t)scaled;
        }
    }
}

/**
 * @brief Player task that decodes and plays audio
 *
 * @param arg  Unused
 */
static void player_task(void *arg)
{
    ESP_LOGI(TAG, "Player task started");

    // Create OGG Opus decoder
    micro_opus::OggOpusDecoder decoder;

    // Track decoded samples for logging
    size_t total_samples = 0;

    // Main decoding loop
    bool file_end = false;
    size_t input_pos = 0;
    size_t input_len = 0;

    while (s_state == PLAYER_PLAYING && !file_end) {
        // Read more data from file if needed
        if (input_len == 0 || input_pos >= input_len) {
            size_t bytes_read = fread(s_input_buf, 1, PLAYER_INPUT_BUF_SIZE, s_file);
            if (bytes_read == 0) {
                // End of file
                file_end = true;
                ESP_LOGI(TAG, "End of file reached");
                break;
            }
            input_len = bytes_read;
            input_pos = 0;
        }

        // Decode OGG Opus data
        size_t bytes_consumed = 0;
        size_t samples_decoded = 0;

        micro_opus::OggOpusResult result = decoder.decode(
            s_input_buf + input_pos,
            input_len - input_pos,
            reinterpret_cast<uint8_t*>(s_output_pcm),
            sizeof(s_output_pcm),
            bytes_consumed,
            samples_decoded
        );

        // Update input position
        input_pos += bytes_consumed;

        // Handle decode result
        if (result < 0) {
            ESP_LOGE(TAG, "Decode error: %d", result);
            break;
        }

        // Process decoded PCM data (48kHz from Opus)
        if (samples_decoded > 0) {
            // Step 1: Resample 48kHz -> 16kHz
            size_t resampled_samples = 0;
            resample_48k_to_16k(s_output_pcm, samples_decoded, s_resample_buf, &resampled_samples);

            // Track resampled samples (16kHz)
            total_samples += resampled_samples;

            // Log every 8 seconds worth of samples (at 16kHz)
            if (total_samples % (16000 * 8) == 0) {
                ESP_LOGI(TAG, "Resampled %zu samples (%zu seconds at 16kHz)",
                         total_samples, total_samples / 16000);
            }

            // Step 2: Convert 16-bit PCM to 32-bit I2S format
            int32_t pcm_32bit[resampled_samples];
            for (size_t i = 0; i < resampled_samples; i++) {
                pcm_32bit[i] = (int32_t)s_resample_buf[i] << 16;  // Left-align 16-bit to 32-bit
            }

            // Step 3: Apply volume scaling
            scale_pcm_samples(pcm_32bit, resampled_samples, s_volume_scale);

            // Step 4: Write to I2S (16kHz)
            size_t bytes_written = 0;
            esp_err_t i2s_ret = i2s_audio_write(s_i2s_handles, pcm_32bit,
                                                 resampled_samples * sizeof(int32_t),
                                                 &bytes_written, portMAX_DELAY);
            if (i2s_ret != ESP_OK) {
                ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(i2s_ret));
                break;
            }
        }
    }

    // Log total samples decoded
    if (total_samples > 0) {
        ESP_LOGI(TAG, "Total samples decoded: %zu (%zu seconds)",
                 total_samples, total_samples / 16000);
    }

    // Cleanup
    s_state = PLAYER_STOPPED;
    if (s_file) {
        fclose(s_file);
        s_file = NULL;
    }

    ESP_LOGI(TAG, "Player stopped");
    vTaskDelete(NULL);
}

esp_err_t player_init(void)
{
    s_state = PLAYER_IDLE;
    s_task_handle = NULL;
    s_file = NULL;
    s_i2s_handles = NULL;
    s_volume_scale = 1.0f;

    ESP_LOGI(TAG, "Player initialized (using micro-opus native decoder)");
    return ESP_OK;
}

void player_deinit(void)
{
    if (s_state == PLAYER_PLAYING) {
        player_stop();
    }

    ESP_LOGI(TAG, "Player deinitialized");
}

esp_err_t player_start(const i2s_audio_handles_t *i2s_handles,
                       const char *filename)
{
    if (s_state == PLAYER_PLAYING) {
        ESP_LOGW(TAG, "Player already playing");
        return ESP_ERR_INVALID_STATE;
    }

    if (i2s_handles == NULL || filename == NULL) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    // Open file
    s_file = fopen(filename, "rb");
    if (s_file == NULL) {
        ESP_LOGE(TAG, "Failed to open file: %s", filename);
        return ESP_FAIL;
    }

    // Get file size for logging
    fseek(s_file, 0, SEEK_END);
    long file_size = ftell(s_file);
    fseek(s_file, 0, SEEK_SET);
    ESP_LOGI(TAG, "File opened: %s, size: %ld bytes", filename, file_size);

    // Save parameters
    s_i2s_handles = i2s_handles;

    // Set state to PLAYING before creating task
    s_state = PLAYER_PLAYING;

    // Create player task
    // Note: Native Opus decoder is stable, only needs 8KB stack
    BaseType_t task_ret = xTaskCreate(player_task, "player",
                                      PLAYER_TASK_STACK_SIZE, NULL, 5, &s_task_handle);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create player task");
        s_state = PLAYER_IDLE;  // Reset state on failure
        fclose(s_file);
        s_file = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Playback started: file=%s", filename);
    return ESP_OK;
}

esp_err_t player_stop(void)
{
    if (s_state != PLAYER_PLAYING) {
        return ESP_ERR_INVALID_STATE;
    }

    // Signal task to stop
    s_state = PLAYER_STOPPING;

    // Wait for task to finish
    vTaskDelay(pdMS_TO_TICKS(100));

    // Force stop if task didn't finish
    if (s_task_handle != NULL) {
        vTaskDelete(s_task_handle);
        s_task_handle = NULL;
    }

    // Close file
    if (s_file) {
        fclose(s_file);
        s_file = NULL;
    }

    s_state = PLAYER_STOPPED;
    ESP_LOGI(TAG, "Player stopped");
    return ESP_OK;
}

player_state_t player_get_state(void)
{
    return s_state;
}

void player_set_volume(float volume)
{
    if (volume < 0.0f) {
        volume = 0.0f;
    } else if (volume > 2.0f) {
        volume = 2.0f;  // Allow up to 200% volume
    }
    s_volume_scale = volume;
    ESP_LOGI(TAG, "Volume set to %.2f", volume);
}

float player_get_volume(void)
{
    return s_volume_scale;
}