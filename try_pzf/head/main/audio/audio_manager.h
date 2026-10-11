/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "audio/i2s_driver.h"
#include "esp_err.h"

typedef enum {
    AUDIO_MANAGER_IDLE,
    AUDIO_MANAGER_RECORDING,
    AUDIO_MANAGER_PLAYING,
    AUDIO_MANAGER_PAUSED,
    AUDIO_MANAGER_ERROR,
} audio_manager_state_t;

/**
 * @brief Initialize audio manager
 *
 * Initializes all audio subsystems:
 * - I2S driver
 * - LittleFS storage
 * - Recorder module
 * - Player module
 *
 * @param[out] i2s_handles  Pointer to store I2S handles
 * @return ESP_OK on success
 */
esp_err_t audio_manager_init(i2s_audio_handles_t *i2s_handles);

/**
 * @brief Deinitialize audio manager
 *
 * Stops all audio operations and releases resources.
 */
void audio_manager_deinit(void);

/**
 * @brief Start recording
 *
 * @param filename     Output filename (e.g., "/audio/rec_001.wav")
 * @param max_duration Maximum recording duration in seconds (0 = unlimited)
 * @return ESP_OK on success
 */
esp_err_t audio_manager_start_record(const char *filename, uint32_t max_duration);

/**
 * @brief Stop recording
 */
esp_err_t audio_manager_stop_record(void);

/**
 * @brief Start playback
 *
 * @param filename  Input filename (e.g., "/audio/music01_opus.ogg")
 * @return ESP_OK on success
 */
esp_err_t audio_manager_start_play(const char *filename);

/**
 * @brief Pause playback
 */
esp_err_t audio_manager_pause(void);

/**
 * @brief Resume playback
 */
esp_err_t audio_manager_resume(void);

/**
 * @brief Stop playback
 */
esp_err_t audio_manager_stop_play(void);

/**
 * @brief Set playback volume (0-100)
 */
esp_err_t audio_manager_set_volume(int volume_percent);

/**
 * @brief Get current state
 */
audio_manager_state_t audio_manager_get_state(void);

/**
 * @brief List all audio files in storage
 *
 * @param[out] count  Number of files found
 * @return ESP_OK on success
 */
esp_err_t audio_manager_list_files(int *count);