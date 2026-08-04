/*
 * tts_http_api.h - TTS HTTP API for remote TTS injection
 */
#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize TTS HTTP API endpoint
 *
 * Registers POST /api/tts/speak on the WebSocket manager's HTTP server.
 * Must be called after ws_manager_server_start().
 *
 * @return ESP_OK on success
 */
esp_err_t tts_http_api_init(void);

#ifdef __cplusplus
}
#endif
