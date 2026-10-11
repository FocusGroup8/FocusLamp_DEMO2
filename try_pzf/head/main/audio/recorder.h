/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "audio/i2s_driver.h"
#include "esp_err.h"

/**
 * @brief WAV file header structure (44 bytes)
 */
typedef struct {
    // RIFF header
    char riff_tag[4];   // "RIFF"
    uint32_t riff_size; // File size - 8
    char wave_tag[4];   // "WAVE"
    // fmt sub-chunk
    char fmt_tag[4];          // "fmt "
    uint32_t fmt_size;        // 16 (PCM)
    uint16_t audio_format;    // 1 (PCM)
    uint16_t num_channels;    // 1 (mono)
    uint32_t sample_rate;     // 16000
    uint32_t byte_rate;       // sample_rate * num_channels * bits_per_sample / 8
    uint16_t block_align;     // num_channels * bits_per_sample / 8
    uint16_t bits_per_sample; // 16
    // data sub-chunk
    char data_tag[4];   // "data"
    uint32_t data_size; // PCM data size in bytes
} wav_header_t;

typedef enum {
    RECORDER_IDLE,
    RECORDER_RECORDING,
    RECORDER_STOPPING,
} recorder_state_t;

/**
 * @brief Initialize recorder module
 *
 * @return ESP_OK on success
 */
esp_err_t recorder_init(void);

/**
 * @brief Deinitialize recorder module
 */
void recorder_deinit(void);

/**
 * @brief Start recording from microphone to WAV file
 *
 * Creates a new WAV file on LittleFS and starts a FreeRTOS task
 * that reads I2S RX data and writes to the file.
 *
 * @param i2s_handles  I2S handles with RX channel
 * @param filename     Output filename (e.g., "/audio/rec_001.wav")
 * @param max_duration Maximum recording duration in seconds (0 = unlimited)
 * @return ESP_OK on success
 */
esp_err_t recorder_start(const i2s_audio_handles_t *i2s_handles, const char *filename, uint32_t max_duration);

/**
 * @brief Stop recording and close WAV file
 *
 * Writes final WAV header with correct data size.
 *
 * @return ESP_OK on success
 */
esp_err_t recorder_stop(void);

/**
 * @brief Get current recorder state
 */
recorder_state_t recorder_get_state(void);