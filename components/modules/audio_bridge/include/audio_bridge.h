#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct audio_bridge_s audio_bridge_t;

/**
 * @brief Audio bridge configuration
 */
typedef struct {
    int sample_rate;         /*!< I2S sample rate (default 16000) */
    int bclk_gpio;           /*!< BCLK GPIO number */
    int ws_gpio;             /*!< WS GPIO number */
    int dout_gpio;           /*!< DOUT GPIO (→ amplifier) */
    int din_gpio;            /*!< DIN GPIO (← microphone) */
} audio_bridge_config_t;

#define AUDIO_BRIDGE_DEFAULT_CONFIG() { \
    .sample_rate = 16000, \
    .bclk_gpio = 32, \
    .ws_gpio = 33, \
    .dout_gpio = 31, \
    .din_gpio = 30, \
}

/**
 * @brief Microphone audio output callback
 *
 * Called by audio_bridge when OPUS-encoded microphone audio is ready.
 * The callee must consume or copy the data before returning.
 *
 * @param opus_data OPUS-encoded audio frame
 * @param len Frame length in bytes
 * @param ctx User context provided in audio_bridge_mic_start()
 */
typedef void (*audio_bridge_mic_callback_t)(const uint8_t *opus_data, int len, void *ctx);

/**
 * @brief Initialize audio bridge (I2S full-duplex)
 */
esp_err_t audio_bridge_init(const audio_bridge_config_t *config);

/**
 * @brief Deinitialize audio bridge
 */
esp_err_t audio_bridge_deinit(void);

/**
 * @brief Write PCM data to I2S TX (speaker playback)
 */
esp_err_t audio_bridge_write_pcm(const void *data, size_t len, size_t *bytes_written, uint32_t timeout_ms);

/**
 * @brief Read PCM data from I2S RX (microphone capture)
 */
esp_err_t audio_bridge_read_pcm(void *data, size_t len, size_t *bytes_read, uint32_t timeout_ms);

/**
 * @brief Feed OPUS-encoded audio data for TTS playback
 *
 * This is designed as the audio_callback for esp_xiaozhi.
 * Decodes OPUS frames and writes PCM to I2S.
 */
void audio_bridge_tts_callback(const uint8_t *data, int len, void *ctx);

/**
 * @brief Set TTS playback volume
 */
esp_err_t audio_bridge_set_volume(int volume_percent);

/**
 * @brief Get current volume
 */
int audio_bridge_get_volume(void);

/**
 * @brief Start microphone capture + OPUS encoding
 *
 * Creates a dedicated mic_task that:
 * 1. Reads PCM from I2S RX (microphone)
 * 2. Encodes to OPUS (16kHz, mono, 60ms frames, VOIP mode)
 * 3. Calls the provided callback with each encoded frame
 *
 * @param callback Function called for each OPUS-encoded frame
 * @param ctx User context passed to callback
 * @return ESP_OK on success
 */
esp_err_t audio_bridge_mic_start(audio_bridge_mic_callback_t callback, void *ctx);

/**
 * @brief Stop microphone capture
 */
esp_err_t audio_bridge_mic_stop(void);

#ifdef __cplusplus
}
#endif
