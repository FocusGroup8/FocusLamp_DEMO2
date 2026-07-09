/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

/**
 * @brief Run complete audio recording-to-playback test
 *
 * Test flow:
 * 1. Initialize audio system (I2S + LittleFS + Recorder + Player)
 * 2. Record 30 seconds of audio to WAV file
 * 3. Wait for recording to complete
 * 4. Read WAV file and play directly (no decoder needed for PCM)
 * 5. Verify the entire pipeline works
 *
 * @return ESP_OK on success
 */
esp_err_t audio_test_rec_to_play(void);

/**
 * @brief Test audio manager initialization
 *
 * Initializes all audio subsystems and verifies they start correctly.
 *
 * @return ESP_OK on success
 */
esp_err_t audio_test_init(void);

/**
 * @brief Test recording functionality
 *
 * Records audio for specified duration and saves to WAV file.
 *
 * @param filename     Output filename (e.g., "/audio/test.wav")
 * @param duration_sec Recording duration in seconds
 * @return ESP_OK on success
 */
esp_err_t audio_test_record(const char *filename, uint32_t duration_sec);

/**
 * @brief Test WAV file playback (direct PCM playback, no decoder)
 *
 * Opens WAV file, reads PCM data, and outputs to I2S TX.
 * Note: This is a simplified WAV player for testing only.
 *
 * @param filename Input WAV filename
 * @return ESP_OK on success
 */
esp_err_t audio_test_play_wav(const char *filename);

/**
 * @brief Test Ogg file playback (using esp_audio_codec decoder)
 *
 * Opens Ogg file, decodes using Simple Decoder API, and outputs to I2S TX.
 *
 * @param filename Input Ogg filename
 * @return ESP_OK on success
 */
esp_err_t audio_test_play_ogg(const char *filename);