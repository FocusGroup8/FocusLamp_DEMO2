/*
 * tts_bridge.h - HTTP bridge to TTS board for voice broadcast
 *
 * Sends text to the TTS board's POST /api/tts/speak endpoint,
 * which plays the text as speech through its speaker.
 */
#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Send text to TTS board for voice playback
 *
 * @param text  UTF-8 text to be spoken
 * @return ESP_OK on success
 */
esp_err_t tts_bridge_speak(const char *text);

#ifdef __cplusplus
}
#endif
