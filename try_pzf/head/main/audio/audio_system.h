/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/**
 * @file audio_system.h
 * @brief Audio system top-level interface for ESP32-P4 I2S audio subsystem
 *
 * This module provides a unified interface for the audio subsystem, including:
 * - I2S full-duplex driver (INMP441 microphone + MAX98357A amplifier)
 * - WAV recorder (microphone → file)
 * - Audio player (file → speaker, supports Opus/Vorbis codecs)
 * - LittleFS storage management
 *
 * The audio system can operate independently without LCD display,
 * supporting the mutually exclusive operation mode with display system.
 */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Audio system operation modes
 */
typedef enum {
    AUDIO_MODE_RECORD_ONLY, /*!< Recording mode: microphone → WAV file */
    AUDIO_MODE_PLAY_ONLY,   /*!< Playback mode: audio file → speaker */
    AUDIO_MODE_LOOPBACK,    /*!< Loopback mode: microphone → speaker */
    AUDIO_MODE_FULL,        /*!< Full mode: recording + playback + monitoring */
} audio_mode_t;

/**
 * @brief Audio system state
 */
typedef enum {
    AUDIO_STATE_IDLE,      /*!< System initialized but not running */
    AUDIO_STATE_RECORDING, /*!< Active recording in progress */
    AUDIO_STATE_PLAYING,   /*!< Active playback in progress */
    AUDIO_STATE_LOOPBACK,  /*!< Active loopback in progress */
    AUDIO_STATE_PAUSED,    /*!< Playback paused */
    AUDIO_STATE_ERROR,     /*!< System error state */
} audio_state_t;

/**
 * @brief Audio system configuration
 */
typedef struct {
    audio_mode_t mode;        /*!< Operation mode (record/play/loopback) */
    uint32_t sample_rate;     /*!< Sample rate in Hz (default: 16000) */
    const char *record_path;  /*!< Recording file path (NULL for default) */
    const char *play_path;    /*!< Playback file path (NULL for default) */
    uint32_t record_duration; /*!< Max recording duration in seconds (0 = unlimited) */
    int volume_percent;       /*!< Initial volume (0-100) */
} audio_config_t;

/**
 * @brief Initialize the audio system
 *
 * Initializes all audio subsystems:
 * - I2S driver (full-duplex channels)
 * - LittleFS storage
 * - Recorder module
 * - Player module with codec support
 *
 * @param config  Audio system configuration (NULL for defaults)
 * @return ESP_OK on success
 */
esp_err_t audio_system_init(const audio_config_t *config);

/**
 * @brief Start audio system in the configured mode
 *
 * Begins audio operation according to the configured mode:
 * - RECORD_ONLY: starts recording task
 * - PLAY_ONLY: starts playback task
 * - LOOPBACK: starts simultaneous recording and playback
 * - FULL: starts full demo with monitoring
 *
 * @return ESP_OK on success
 */
esp_err_t audio_system_start(void);

/**
 * @brief Stop audio system operation
 *
 * Stops any active recording, playback, or loopback operation.
 * Does not deinitialize the system (can be restarted).
 *
 * @return ESP_OK on success
 */
esp_err_t audio_system_stop(void);

/**
 * @brief Pause playback (only valid in PLAY or LOOPBACK mode)
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not playing
 */
esp_err_t audio_system_pause(void);

/**
 * @brief Resume playback from paused state
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not paused
 */
esp_err_t audio_system_resume(void);

/**
 * @brief Set playback volume (0-100)
 *
 * @param volume_percent  Volume level (0 = mute, 100 = max)
 * @return ESP_OK on success
 */
esp_err_t audio_system_set_volume(int volume_percent);

/**
 * @brief Get current audio system state
 *
 * @return Current state
 */
audio_state_t audio_system_get_state(void);

/**
 * @brief Deinitialize audio system
 *
 * Stops all operations and releases all resources:
 * - I2S channels
 * - LittleFS storage
 * - Recorder and player modules
 */
void audio_system_deinit(void);

#ifdef __cplusplus
}
#endif