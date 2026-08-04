/*
 * audio_driver.h - Audio driver for FocusLamp
 * I2S-based audio playback and capture.
 */

#pragma once
#ifndef __AUDIO_DRIVER_H__
#define __AUDIO_DRIVER_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Audio driver state */
typedef enum {
    AUDIO_STATE_UNINIT = 0,
    AUDIO_STATE_IDLE,
    AUDIO_STATE_PLAYING,
    AUDIO_STATE_RECORDING,
    AUDIO_STATE_LOOPBACK,
    AUDIO_STATE_ERROR,
} audio_state_t;

/** Audio driver status for querying */
typedef struct {
    audio_state_t state;
    int sample_rate;
    float volume;
    float gain;
    bool noise_reduction_enabled;
    int noise_threshold;
} audio_driver_status_t;

/**
 * @brief Initialize audio driver.
 * @return esp_err_t
 */
esp_err_t audio_driver_init(void);

/**
 * @brief Deinitialize audio driver.
 * @return esp_err_t
 */
esp_err_t audio_driver_deinit(void);

/**
 * @brief Start audio playback.
 * @return esp_err_t
 */
esp_err_t audio_driver_start(void);

/**
 * @brief Stop audio playback.
 * @return esp_err_t
 */
esp_err_t audio_driver_stop(void);

/**
 * @brief Write audio data (PCM) to the I2S output.
 * @param data  Pointer to PCM data buffer
 * @param len   Data length in bytes
 * @return esp_err_t
 */
esp_err_t audio_driver_write(const uint8_t *data, size_t len);

/**
 * @brief Read audio data (PCM) from the I2S input.
 * @param data  Pointer to receive buffer
 * @param len   Buffer size in bytes
 * @return int  Number of bytes read, or -1 on error
 */
int audio_driver_read(uint8_t *data, size_t len);

/**
 * @brief Set playback volume.
 * @param vol  Volume level (0-100)
 * @return esp_err_t
 */
esp_err_t audio_driver_set_volume(uint8_t vol);

/**
 * @brief Get current volume.
 * @return uint8_t Current volume (0-100)
 */
uint8_t audio_driver_get_volume(void);

/**
 * @brief Set mute state.
 * @param mute  true = mute, false = unmute
 * @return esp_err_t
 */
esp_err_t audio_driver_set_mute(bool mute);

/**
 * @brief Get current audio driver state.
 * @return audio_state_t
 */
audio_state_t audio_driver_get_state(void);

/**
 * @brief Set gain value.
 * @param gain Gain (0.1 - 10.0)
 * @return esp_err_t
 */
esp_err_t audio_driver_set_gain(float gain);

/**
 * @brief Set noise reduction.
 * @param enable    true to enable
 * @param threshold noise gate threshold
 * @return esp_err_t
 */
esp_err_t audio_driver_set_noise_reduction(bool enable, int threshold);

/**
 * @brief Get full driver status.
 * @param status Output status structure
 * @return esp_err_t
 */
esp_err_t audio_driver_get_status(audio_driver_status_t *status);

/**
 * @brief Start audio loopback (mic → speaker).
 * @return esp_err_t
 */
esp_err_t audio_driver_start_loopback(void);

/**
 * @brief Stop audio loopback.
 * @return esp_err_t
 */
esp_err_t audio_driver_stop_loopback(void);

/**
 * @brief Start recording.
 * @return esp_err_t
 */
esp_err_t audio_driver_start_recording(void);

/**
 * @brief Stop recording.
 * @return esp_err_t
 */
esp_err_t audio_driver_stop_recording(void);

/**
 * @brief Start playback (explicit).
 * @return esp_err_t
 */
esp_err_t audio_driver_start_playback(void);

/**
 * @brief Stop playback (explicit).
 * @return esp_err_t
 */
esp_err_t audio_driver_stop_playback(void);

/**
 * @brief Play a sine wave tone.
 * @param freq_hz     Tone frequency in Hz (e.g. 440 for A4)
 * @param duration_ms Duration in ms. 0 = start continuous, call audio_stop_tone() to stop.
 * @return esp_err_t
 */
esp_err_t audio_driver_play_tone(uint16_t freq_hz, uint32_t duration_ms);

/**
 * @brief Stop any ongoing tone playback.
 * @return esp_err_t
 */
esp_err_t audio_driver_stop_tone(void);

#ifdef __cplusplus
}
#endif

#endif /* __AUDIO_DRIVER_H__ */
