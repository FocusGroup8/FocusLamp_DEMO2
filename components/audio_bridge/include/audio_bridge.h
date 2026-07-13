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
 * @brief Initialize audio bridge (I2S full-duplex)
 */
esp_err_t audio_bridge_init(const audio_bridge_config_t *config);

/**
 * @brief Deinitialize audio bridge
 */
esp_err_t audio_bridge_deinit(void);

/**
 * @brief Write PCM data to I2S TX (speaker playback)
 * @param data PCM data (16-bit signed, mono, at configured sample rate)
 * @param len Data length in bytes
 * @param bytes_written Output: actual bytes written
 * @param timeout_ms Timeout (portMAX_DELAY for infinite)
 */
esp_err_t audio_bridge_write_pcm(const void *data, size_t len, size_t *bytes_written, uint32_t timeout_ms);

/**
 * @brief Read PCM data from I2S RX (microphone capture)
 * @param data Output buffer
 * @param len Buffer length
 * @param bytes_read Output: actual bytes read
 * @param timeout_ms Timeout
 */
esp_err_t audio_bridge_read_pcm(void *data, size_t len, size_t *bytes_read, uint32_t timeout_ms);

/**
 * @brief Feed OPUS-encoded audio data for TTS playback
 *
 * This is designed as the audio_callback for esp_xiaozhi.
 * Decodes OPUS frames and writes PCM to I2S.
 *
 * @param data OPUS-encoded audio data
 * @param len Data length in bytes
 * @param ctx User context (unused)
 */
void audio_bridge_tts_callback(const uint8_t *data, int len, void *ctx);

/**
 * @brief Set TTS playback volume
 * @param volume_percent Volume 0-100
 */
esp_err_t audio_bridge_set_volume(int volume_percent);

/**
 * @brief Get current volume
 * @return Volume 0-100
 */
int audio_bridge_get_volume(void);

#ifdef __cplusplus
}
#endif
