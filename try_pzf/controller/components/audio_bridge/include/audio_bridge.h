#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct audio_bridge_s audio_bridge_t;

/** Lifetime counters. peak is the absolute PCM16 maximum since last snapshot. */
typedef struct {
  uint32_t read_calls, rx_frames, read_errors, partial_reads;
  uint32_t encode_calls, encoded_frames, encode_errors, empty_encoded;
  uint32_t peak;
} audio_bridge_mic_diagnostics_t;
void audio_bridge_get_mic_diagnostics(audio_bridge_mic_diagnostics_t *out);

/**
 * @brief Audio bridge configuration
 */
typedef struct {
  int sample_rate; /*!< I2S sample rate (default 16000) */
  int bclk_gpio;   /*!< BCLK GPIO number */
  int ws_gpio;     /*!< WS GPIO number */
  int dout_gpio;   /*!< DOUT GPIO (→ amplifier) */
  int din_gpio;    /*!< DIN GPIO (← microphone) */
} audio_bridge_config_t;

#define AUDIO_BRIDGE_DEFAULT_CONFIG()                                          \
  {                                                                            \
      .sample_rate = 16000,                                                    \
      .bclk_gpio = 32,                                                         \
      .ws_gpio = 33,                                                           \
      .dout_gpio = 31,                                                         \
      .din_gpio = 30,                                                          \
  }

/**
 * @brief Microphone audio output callback (OPUS-encoded)
 *
 * Called by audio_bridge when OPUS-encoded microphone audio is ready.
 * The callee must consume or copy the data before returning.
 *
 * @param opus_data OPUS-encoded audio frame
 * @param len Frame length in bytes
 * @param ctx User context provided in audio_bridge_mic_start()
 */
typedef void (*audio_bridge_mic_callback_t)(const uint8_t *opus_data, int len,
                                            void *ctx);

/**
 * @brief Raw PCM audio data callback (16-bit, 16kHz, mono)
 *
 * Called by audio_bridge when raw PCM microphone data is available,
 * after I2S 32-bit→16-bit down-sampling but before OPUS encoding.
 * Used by wake word engine (ESP-SR) to feed AFE/MultiNet.
 *
 * @param pcm_data  16-bit signed PCM samples (16kHz mono, 960 samples/frame)
 * @param sample_count Number of samples in this frame
 * @param ctx User context provided in audio_bridge_register_pcm_callback()
 */
typedef void (*audio_bridge_pcm_callback_t)(const int16_t *pcm_data,
                                            int sample_count, void *ctx);

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
esp_err_t audio_bridge_write_pcm(const void *data, size_t len,
                                 size_t *bytes_written, uint32_t timeout_ms);

/* Blocking, exclusive 16kHz mono little-endian PCM16 playback. Call from a
 * dedicated task. App must pause recognition while playing. ESP_OK means all
 * data plus a DMA-ring-sized silence tail was written, not physical audibility. */
esp_err_t audio_bridge_play_local_pcm16(const uint8_t *data, size_t len);
/* Called synchronously by the playback task; return true to cancel. The
 * callback must not block. Cancellation returns ESP_ERR_INVALID_STATE after
 * clearing queued local output and releasing playback ownership.
 * output_started, if non-NULL, reports whether a PCM chunk was written
 * successfully, even when playback later fails or is cancelled. */
typedef bool (*audio_bridge_cancel_cb_t)(void *ctx);
esp_err_t audio_bridge_play_local_pcm16_cancellable(
    const uint8_t *data, size_t len, audio_bridge_cancel_cb_t should_cancel,
    void *ctx, bool *output_started);
bool audio_bridge_local_playing(void);

/**
 * @brief Read PCM data from I2S RX (microphone capture)
 */
esp_err_t audio_bridge_read_pcm(void *data, size_t len, size_t *bytes_read,
                                uint32_t timeout_ms);

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
 * @brief Flush pending TTS OPUS frames from decode queue
 *
 * Called when TTS playback stops to discard any stale OPUS frames
 * still queued for decoding. Without this, leftover frames continue
 * to write to I2S after TTS has ended, causing ESP_ERR_TIMEOUT
 * errors as the DMA buffers drain slowly.
 */
void audio_bridge_flush_tts(void);

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
esp_err_t audio_bridge_mic_start(audio_bridge_mic_callback_t callback,
                                 void *ctx);

/**
 * @brief Stop microphone capture
 */
esp_err_t audio_bridge_mic_stop(void);

/**
 * @brief Register a raw PCM data callback for wake word engine
 *
 * The callback is called from the mic task context each time a PCM frame
 * is available (960 samples, 16kHz, mono, 60ms). This must be called
 * BEFORE audio_bridge_mic_start() for the callback to take effect.
 *
 * @param callback Function called for each PCM frame (NULL to unregister)
 * @param ctx User context passed to callback
 */
void audio_bridge_register_pcm_callback(audio_bridge_pcm_callback_t callback,
                                        void *ctx);

/** Detach PCM callback and wait without holding a lock for admitted calls to finish.
 * Returns ESP_ERR_TIMEOUT if consumer resources must be retained; cannot be called
 * from the microphone task itself. */
esp_err_t audio_bridge_unregister_pcm_callback_and_wait(uint32_t timeout_ms);

/**
 * @brief Read TTS reference PCM data for AEC (Acoustic Echo Cancellation)
 *
 * Reads 16-bit PCM data that was written to I2S TX (speaker) by the TTS
 * decode task. This reference signal is fed to ESP-SR AFE's AEC module
 * alongside microphone input to cancel echo during wake word detection.
 *
 * The data is read from an internal ring buffer filled by write_pcm_to_i2s().
 * If insufficient data is available, the output buffer is zero-filled.
 *
 * @param out_buf    Output buffer for 16-bit PCM reference samples
 * @param samples    Number of samples to read
 * @param timeout_ms Timeout in ms to wait for data (0 = non-blocking)
 * @return Number of samples actually read, or -1 on error
 */
int audio_bridge_read_ref_pcm(int16_t *out_buf, int samples,
                              uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
