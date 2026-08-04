/*
 * audio_service.h - Audio service for FocusLamp
 */

#pragma once
#ifndef __AUDIO_SERVICE_H__
#define __AUDIO_SERVICE_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== Audio State Enumeration ===================== */
typedef enum {
    AUDIO_SERVICE_STATE_IDLE    = 0,
    AUDIO_SERVICE_STATE_PLAYING,
    AUDIO_SERVICE_STATE_PAUSED,
    AUDIO_SERVICE_STATE_ERROR,
} audio_service_state_t;

/* ===================== Volume Level Definitions ===================== */
#define AUDIO_VOLUME_LEVEL_MIN    0
#define AUDIO_VOLUME_LEVEL_MAX    5
#define AUDIO_VOLUME_LEVEL_COUNT  6

/**
 * @brief Initialize audio service.
 *        Subscribes to EV_AUDIO_* events and initializes audio_driver.
 * @return esp_err_t
 */
esp_err_t audio_service_init(void);

/**
 * @brief Play audio from URI (file path or URL).
 * @param uri  URI string identifying the audio source
 * @return esp_err_t
 */
esp_err_t audio_service_play(const char *uri);

/**
 * @brief Stop current audio playback.
 * @return esp_err_t
 */
esp_err_t audio_service_stop(void);

/**
 * @brief Pause current audio playback.
 * @return esp_err_t
 */
esp_err_t audio_service_pause(void);

/**
 * @brief Resume paused audio playback.
 * @return esp_err_t
 */
esp_err_t audio_service_resume(void);

/**
 * @brief Set playback volume.
 * @param vol  Volume level (0-100)
 * @return esp_err_t
 */
esp_err_t audio_service_set_volume(uint8_t vol);

/**
 * @brief Set mute state.
 * @param mute  true = mute, false = unmute
 * @return esp_err_t
 */
esp_err_t audio_service_set_mute(bool mute);

/**
 * @brief Get current audio service state.
 * @return audio_service_state_t
 */
audio_service_state_t audio_service_get_state(void);

/**
 * @brief Set volume level (0-5).
 * @param level  Volume level, 0 = mute, 5 = max
 * @return esp_err_t
 */
esp_err_t audio_service_set_volume_level(uint8_t level);

/**
 * @brief Get current volume level (0-5).
 * @return uint8_t Current volume level
 */
uint8_t audio_service_get_volume_level(void);

/**
 * @brief Increase volume by one level.
 * @return esp_err_t
 */
esp_err_t audio_service_volume_up(void);

/**
 * @brief Decrease volume by one level.
 * @return esp_err_t
 */
esp_err_t audio_service_volume_down(void);

/**
 * @brief Play a tone/alert.
 * @param freq_hz     Tone frequency in Hz
 * @param duration_ms Duration in milliseconds
 * @return esp_err_t
 */
esp_err_t audio_service_play_tone(uint16_t freq_hz, uint32_t duration_ms);

/**
 * @brief Play a short alert beep.
 * @return esp_err_t
 */
esp_err_t audio_service_play_alert(void);

/**
 * @brief Play a PCM/WAV audio file from filesystem.
 *        Currently supports raw PCM files; WAV header is skipped if present.
 * @param path  Absolute file path
 * @return esp_err_t
 */
esp_err_t audio_service_play_file(const char *path);

/**
 * @brief Play TTS for the given text.
 *        If a pre-generated audio file exists, play it; otherwise not supported.
 * @param text  Text to speak
 * @return esp_err_t
 */
esp_err_t audio_service_play_tts(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* __AUDIO_SERVICE_H__ */