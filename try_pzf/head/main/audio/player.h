/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "audio/i2s_driver.h"
#include "esp_err.h"

// C++ compatibility: allow C files to call C++ functions
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PLAYER_IDLE,
    PLAYER_PLAYING,
    PLAYER_PAUSED,
    PLAYER_STOPPING,
    PLAYER_STOPPED,
} player_state_t;

/**
 * @brief Initialize player module
 *
 * Registers default decoders (Opus, Vorbis) from esp_audio_codec.
 *
 * @return ESP_OK on success
 */
esp_err_t player_init(void);

/**
 * @brief Deinitialize player module
 */
void player_deinit(void);

/**
 * @brief Start playing an Ogg audio file
 *
 * Opens the Ogg file from LittleFS, decodes using esp_audio_codec,
 * and writes PCM data to I2S TX channel.
 *
 * @param i2s_handles  I2S handles with TX channel
 * @param filename     Input filename (e.g., "/audio/music01_opus.ogg")
 * @return ESP_OK on success
 */
esp_err_t player_start(const i2s_audio_handles_t *i2s_handles, const char *filename);

/**
 * @brief Pause playback
 */
esp_err_t player_pause(void);

/**
 * @brief Resume playback
 */
esp_err_t player_resume(void);

/**
 * @brief Stop playback
 */
esp_err_t player_stop(void);

/**
 * @brief Get current player state
 */
player_state_t player_get_state(void);

/**
 * @brief Set playback volume (0-100)
 */
esp_err_t player_set_volume(int volume_percent);

#ifdef __cplusplus
}
#endif