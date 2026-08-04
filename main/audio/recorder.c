/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "recorder.h"

#include "board_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "stdio.h"
#include "system_monitor.h"

#include <string.h>

static const char *TAG = "RECORDER";

// Recorder state
static recorder_state_t s_state                 = RECORDER_IDLE;
static TaskHandle_t s_task_handle               = NULL;
static SemaphoreHandle_t s_stop_mutex           = NULL;
static FILE *s_file                             = NULL;
static const i2s_audio_handles_t *s_i2s_handles = NULL;
static uint32_t s_max_duration                  = 0;
static uint32_t s_recorded_samples              = 0;

// I2S read buffer (32-bit samples from INMP441)
#define RECORDER_I2S_BUF_SIZE 1024 // Number of 32-bit samples
static int32_t s_i2s_buf[RECORDER_I2S_BUF_SIZE];

// WAV PCM buffer (16-bit samples after conversion)
#define RECORDER_PCM_BUF_SIZE RECORDER_I2S_BUF_SIZE
static int16_t s_pcm_buf[RECORDER_PCM_BUF_SIZE];

/**
 * @brief Initialize WAV header with placeholder values
 */
static void wav_header_init(wav_header_t *header, uint32_t sample_rate)
{
    memcpy(header->riff_tag, "RIFF", 4);
    header->riff_size = 0; // Placeholder, will be updated on stop
    memcpy(header->wave_tag, "WAVE", 4);
    memcpy(header->fmt_tag, "fmt ", 4);
    header->fmt_size        = 16;
    header->audio_format    = 1; // PCM
    header->num_channels    = 1; // Mono
    header->sample_rate     = sample_rate;
    header->bits_per_sample = 16;
    header->byte_rate       = sample_rate * header->num_channels * header->bits_per_sample / 8;
    header->block_align     = header->num_channels * header->bits_per_sample / 8;
    memcpy(header->data_tag, "data", 4);
    header->data_size = 0; // Placeholder, will be updated on stop
}

/**
 * @brief Update WAV header with final data size
 */
static void wav_header_update(FILE *file, uint32_t data_size)
{
    wav_header_t header;
    header.riff_size = data_size + sizeof(wav_header_t) - 8;
    header.data_size = data_size;

    // Seek to beginning and rewrite header
    fseek(file, 0, SEEK_SET);
    fwrite(&header, sizeof(wav_header_t), 1, file);
    fflush(file);
}

/**
 * @brief Convert 32-bit INMP441 samples to 16-bit PCM
 *
 * INMP441 outputs 24-bit data left-aligned in 32-bit frame:
 *   - High 24 bits: valid audio data
 *   - Low 8 bits: zero padding
 *
 * To get 16-bit PCM, we right-shift by 16 bits and take the upper 16 bits.
 */
static void convert_32bit_to_16bit(const int32_t *input, int16_t *output, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        // Right-shift by 16 to extract upper 16 bits from 24-bit valid data
        // The 24-bit data is in bits [31:8], shift right by 16 gives bits [15:0]
        output[i] = (int16_t)(input[i] >> 16);
    }
}

/**
 * @brief Recorder task function
 */
static void recorder_task(void *arg)
{
    ESP_LOGI(TAG, "Recorder task started");

    size_t bytes_read          = 0;
    size_t samples_read        = 0;
    size_t samples_written     = 0;
    uint32_t elapsed_ms        = 0;
    const uint32_t sample_rate = BOARD_I2S_SAMPLE_RATE;

    while (s_state == RECORDER_RECORDING) {
        // Read from I2S RX (blocking, timeout 1000ms)
        esp_err_t ret = i2s_audio_read(s_i2s_handles, s_i2s_buf, sizeof(s_i2s_buf), &bytes_read, 1000);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S read failed: %s", esp_err_to_name(ret));
            break;
        }

        if (bytes_read == 0) {
            ESP_LOGW(TAG, "I2S read timeout, no data");
            continue;
        }

        // Convert samples count (bytes to 32-bit samples)
        samples_read = bytes_read / sizeof(int32_t);

        // Convert 32-bit to 16-bit PCM
        convert_32bit_to_16bit(s_i2s_buf, s_pcm_buf, samples_read);

        // Write PCM data to WAV file
        samples_written = fwrite(s_pcm_buf, sizeof(int16_t), samples_read, s_file);
        if (samples_written != samples_read) {
            ESP_LOGE(TAG, "File write failed: expected %u, wrote %u", (unsigned)samples_read,
                     (unsigned)samples_written);
            break;
        }

        // Update counters
        s_recorded_samples += samples_read;

        // Calculate elapsed time
        elapsed_ms = (s_recorded_samples * 1000) / sample_rate;

        // Check max duration
        if (s_max_duration > 0 && elapsed_ms >= s_max_duration * 1000) {
            ESP_LOGI(TAG, "Reached max duration (%u seconds), stopping", s_max_duration);
            break;
        }

        // Periodically log progress (every 5 seconds)
        if (elapsed_ms > 0 && elapsed_ms % 5000 == 0) {
            ESP_LOGI(TAG, "Recording: %u seconds, %u samples", elapsed_ms / 1000, (unsigned)s_recorded_samples);
        }
    }

    // Update WAV header with final data size
    uint32_t pcm_data_size = s_recorded_samples * sizeof(int16_t);
    wav_header_update(s_file, pcm_data_size);

    // Close file
    fclose(s_file);
    s_file = NULL;

    ESP_LOGI(TAG, "Recording stopped: %u samples (%u seconds), file size %u bytes", (unsigned)s_recorded_samples,
             elapsed_ms / 1000, (unsigned)(pcm_data_size + sizeof(wav_header_t)));

    // Update system monitor with recording duration
    sysmon_update_rec_duration(elapsed_ms);
    ESP_LOGI(TAG, "Updated recording duration: %lu ms", (unsigned long)elapsed_ms);

    // Update state
    s_state       = RECORDER_IDLE;
    s_task_handle = NULL;

    vTaskDelete(NULL);
}

esp_err_t recorder_init(void)
{
    s_state            = RECORDER_IDLE;
    s_task_handle      = NULL;
    s_file             = NULL;
    s_i2s_handles      = NULL;
    s_max_duration     = 0;
    s_recorded_samples = 0;

    // Create mutex for stop synchronization
    s_stop_mutex = xSemaphoreCreateMutex();
    if (s_stop_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Recorder initialized");
    return ESP_OK;
}

void recorder_deinit(void)
{
    if (s_state == RECORDER_RECORDING) {
        recorder_stop();
    }

    if (s_stop_mutex) {
        vSemaphoreDelete(s_stop_mutex);
        s_stop_mutex = NULL;
    }

    ESP_LOGI(TAG, "Recorder deinitialized");
}

esp_err_t recorder_start(const i2s_audio_handles_t *i2s_handles, const char *filename, uint32_t max_duration)
{
    if (i2s_handles == NULL || filename == NULL) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_state != RECORDER_IDLE) {
        ESP_LOGE(TAG, "Recorder not idle, current state: %d", s_state);
        return ESP_ERR_INVALID_STATE;
    }

    // Create WAV file
    s_file = fopen(filename, "wb");
    if (s_file == NULL) {
        ESP_LOGE(TAG, "Failed to create file: %s", filename);
        return ESP_FAIL;
    }

    // Write placeholder WAV header
    wav_header_t header;
    wav_header_init(&header, BOARD_I2S_SAMPLE_RATE);
    size_t written = fwrite(&header, sizeof(wav_header_t), 1, s_file);
    if (written != 1) {
        ESP_LOGE(TAG, "Failed to write WAV header");
        fclose(s_file);
        s_file = NULL;
        return ESP_FAIL;
    }
    fflush(s_file);

    // Save parameters
    s_i2s_handles      = i2s_handles;
    s_max_duration     = max_duration;
    s_recorded_samples = 0;

    // Set state BEFORE creating task to ensure task enters the loop immediately
    s_state = RECORDER_RECORDING;

    // Create recorder task
    BaseType_t ret = xTaskCreate(recorder_task, "recorder", 4096, NULL, 5, &s_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create recorder task");
        fclose(s_file);
        s_file  = NULL;
        s_state = RECORDER_IDLE;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Recording started: file=%s, max_duration=%u sec", filename, max_duration);
    return ESP_OK;
}

esp_err_t recorder_stop(void)
{
    // If already idle, the task stopped naturally (e.g., max duration reached)
    if (s_state == RECORDER_IDLE) {
        ESP_LOGI(TAG, "Recorder already idle (stopped naturally)");
        return ESP_OK;
    }

    if (s_state != RECORDER_RECORDING) {
        ESP_LOGW(TAG, "Recorder not recording, current state: %d", s_state);
        return ESP_ERR_INVALID_STATE;
    }

    // Signal task to stop
    s_state = RECORDER_STOPPING;

    // Wait for task to finish
    if (s_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "Recorder stopped");
    return ESP_OK;
}

recorder_state_t recorder_get_state(void)
{
    return s_state;
}