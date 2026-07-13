/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "esp_xiaozhi_chat.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Xiaozhi manager event types
 */
typedef enum {
    XIAOZHI_MANAGER_EVENT_CONNECTED,            /*!< Connected to xiaozhi server */
    XIAOZHI_MANAGER_EVENT_DISCONNECTED,         /*!< Disconnected from xiaozhi server */
    XIAOZHI_MANAGER_EVENT_AUDIO_CHANNEL_OPENED, /*!< Audio channel opened */
    XIAOZHI_MANAGER_EVENT_AUDIO_CHANNEL_CLOSED, /*!< Audio channel closed */
    XIAOZHI_MANAGER_EVENT_TTS_START,            /*!< TTS playback started */
    XIAOZHI_MANAGER_EVENT_TTS_STOP,             /*!< TTS playback stopped */
    XIAOZHI_MANAGER_EVENT_TTS_SENTENCE,         /*!< TTS sentence received */
    XIAOZHI_MANAGER_EVENT_STT_TEXT,             /*!< STT text received */
    XIAOZHI_MANAGER_EVENT_ERROR,               /*!< Error occurred */
} xiaozhi_manager_event_t;

/**
 * @brief  Xiaozhi manager state
 */
typedef enum {
    XIAOZHI_MANAGER_STATE_IDLE,         /*!< Not initialized */
    XIAOZHI_MANAGER_STATE_INITIALIZED,  /*!< Initialized but not connected */
    XIAOZHI_MANAGER_STATE_CONNECTING,   /*!< Connecting to server */
    XIAOZHI_MANAGER_STATE_CONNECTED,    /*!< Connected to server */
    XIAOZHI_MANAGER_STATE_LISTENING,    /*!< Listening for voice input */
    XIAOZHI_MANAGER_STATE_SPEAKING,     /*!< Speaking (TTS playing) */
    XIAOZHI_MANAGER_STATE_ERROR,        /*!< Error state */
} xiaozhi_manager_state_t;

/**
 * @brief  Callback for xiaozhi manager events
 *
 * @param event   Event type
 * @param data    Event data (depends on event type)
 * @param ctx     User context
 */
typedef void (*xiaozhi_manager_event_cb_t)(xiaozhi_manager_event_t event, void *data, void *ctx);

/**
 * @brief  Callback for receiving TTS audio data
 *
 * @param data    Audio data buffer (valid only during callback)
 * @param len     Data length in bytes
 * @param ctx     User context
 */
typedef void (*xiaozhi_manager_audio_cb_t)(const uint8_t *data, int len, void *ctx);

/**
 * @brief  Xiaozhi manager configuration
 */
typedef struct {
    xiaozhi_manager_event_cb_t event_cb;        /*!< Event callback */
    xiaozhi_manager_audio_cb_t audio_cb;        /*!< Audio data callback (for TTS playback) */
    void *event_cb_ctx;                         /*!< Event callback context */
    void *audio_cb_ctx;                         /*!< Audio callback context */
} xiaozhi_manager_config_t;

/**
 * @brief  Default configuration
 */
#define XIAOZHI_MANAGER_DEFAULT_CONFIG() { \
    .event_cb = NULL, \
    .audio_cb = NULL, \
    .event_cb_ctx = NULL, \
    .audio_cb_ctx = NULL, \
}

/**
 * @brief  Initialize xiaozhi manager
 *
 * Initializes esp_xiaozhi chat module, creates MCP engine,
 * registers MCP tools from all sub-modules (task, light, arm, speaker).
 *
 * @param config  Configuration
 * @return ESP_OK on success
 */
esp_err_t xiaozhi_manager_init(const xiaozhi_manager_config_t *config);

/**
 * @brief  Deinitialize xiaozhi manager
 *
 * Stops chat session and releases all resources.
 *
 * @return ESP_OK on success
 */
esp_err_t xiaozhi_manager_deinit(void);

/**
 * @brief  Start xiaozhi chat session
 *
 * Connects to xiaozhi server and starts the chat session.
 * Requires WiFi connection before calling.
 *
 * @return ESP_OK on success
 */
esp_err_t xiaozhi_manager_start(void);

/**
 * @brief  Stop xiaozhi chat session
 *
 * @return ESP_OK on success
 */
esp_err_t xiaozhi_manager_stop(void);

/**
 * @brief  Get current manager state
 *
 * @return Current state
 */
xiaozhi_manager_state_t xiaozhi_manager_get_state(void);

/**
 * @brief  Send wake word detected
 *
 * Should be called when a wake word is detected by ESP-SR or button.
 *
 * @param wake_word  Wake word string (e.g., "你好小智", "Focus")
 * @return ESP_OK on success
 */
esp_err_t xiaozhi_manager_send_wake_word(const char *wake_word);

/**
 * @brief  Open audio channel manually
 *
 * Opens the audio channel for sending audio data.
 * Typically called after wake word detection.
 *
 * @return ESP_OK on success
 */
esp_err_t xiaozhi_manager_open_audio_channel(void);

/**
 * @brief  Close audio channel
 *
 * @return ESP_OK on success
 */
esp_err_t xiaozhi_manager_close_audio_channel(void);

/**
 * @brief  Send audio data to server
 *
 * @param data      Audio data (OPUS encoded)
 * @param data_len  Data length
 * @return ESP_OK on success
 */
esp_err_t xiaozhi_manager_send_audio(const char *data, size_t data_len);

/**
 * @brief  Get MCP engine handle
 *
 * Returns the MCP engine for external tool registration.
 *
 * @return MCP engine handle, or NULL if not initialized
 */
esp_mcp_t *xiaozhi_manager_get_mcp_engine(void);

/**
 * @brief  Inject text for TTS playback via MCP notification.speak tool
 *
 * Used for proactive TTS injection (e.g., posture reminder, focus reminder).
 * The text will be sent to the server and TTS audio will be returned.
 *
 * @param text      Text to speak
 * @param priority  Priority level (0-3, higher = can interrupt lower)
 * @return ESP_OK on success
 */
esp_err_t xiaozhi_manager_speak(const char *text, int priority);

#ifdef __cplusplus
}
#endif
