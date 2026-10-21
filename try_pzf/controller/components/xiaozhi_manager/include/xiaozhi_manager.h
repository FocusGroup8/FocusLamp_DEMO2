/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include "esp_xiaozhi_chat.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Lifetime counters; gate counters overlap when several conditions block.
 * send_ok/listen_ok mean local API success, not server acknowledgement. */
typedef struct {
    uint32_t callbacks, blocked_state, blocked_tts, blocked_local;
    uint32_t send_attempts, send_ok, send_failed;
    uint32_t send_cancelled; /**< Legacy counter; async sender is never force-deleted. */
    uint32_t listen_attempts, listen_ok, listen_failed;
    esp_err_t last_send_error, last_listen_error;
    int64_t send_started_us; /**< Zero when the last entered upload returned. */
    int64_t send_inflight_ms; /**< Snapshot age, not an internal transport phase. */
    int64_t cloud_event_age_ms; /**< Selected cloud business callbacks, not all WS RX; -1 if unseen. */
    int64_t audio_rx_age_ms; /**< Last valid downlink audio callback; -1 if unseen. */
    int64_t queue_dwell_ms; /**< Last dequeued upload's queue wait, not send time; -1 if unseen. */
    uint32_t enqueued, dropped_oldest, discarded, rejected_frames;
    uint32_t queued, queue_high_water, generation, chat_users;
    uint32_t sender_stack_free; /**< ESP-IDF stack high-water mark, bytes. */
    bool retiring;
} xiaozhi_mic_diagnostics_t;
void xiaozhi_manager_get_mic_diagnostics(xiaozhi_mic_diagnostics_t *out);

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
    XIAOZHI_MANAGER_EVENT_SERVER_GOODBYE,       /*!< Server sent goodbye (conversation ended) */
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
    bool auto_reconnect;                        /*!< Enable automatic reconnection on disconnect/error */
    int reconnect_delay_ms;                     /*!< Initial reconnection delay in ms (default 3000) */
    int reconnect_max_delay_ms;                 /*!< Maximum reconnection delay in ms (default 60000) */
    int reconnect_max_retries;                  /*!< Maximum reconnection attempts, 0 = unlimited (default 0) */
} xiaozhi_manager_config_t;

/**
 * @brief  Default configuration
 */
#define XIAOZHI_MANAGER_DEFAULT_CONFIG() { \
    .event_cb = NULL, \
    .audio_cb = NULL, \
    .event_cb_ctx = NULL, \
    .audio_cb_ctx = NULL, \
    .auto_reconnect = true, \
    .reconnect_delay_ms = 3000, \
    .reconnect_max_delay_ms = 60000, \
    .reconnect_max_retries = 0, \
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
 * @brief  Interrupt current TTS playback (barge-in)
 *
 * Aborts the current speaking if TTS is playing. Called by the wake word
 * engine's barge-in VAD when user speech is detected during playback, so
 * the user can take over the conversation.
 *
 * @return ESP_OK on success (or if not currently speaking)
 */
esp_err_t xiaozhi_manager_interrupt_speaking(void);

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
 * @brief  Copy audio into the bounded asynchronous upload queue
 *
 * @param data      Audio data (OPUS encoded)
 * @param data_len  Data length
 * @return ESP_OK if queued (not a server delivery acknowledgement).
 * Requires active listening; full queue drops the oldest pending frame.
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
 * @brief  Inject text for TTS playback via proactive injection mechanism
 *
 * Used for proactive TTS injection (e.g., posture reminder, focus reminder).
 * The text will be sent as a short command to the xiaozhi server,
 * which processes it through LLM and returns TTS audio.
 *
 * Implementation: submit text as listen/detect in an exclusive broadcast
 * session. Normal dialogues are not interrupted; accepted requests wait in
 * the existing FIFO. Cloud response/audio output is not guaranteed.
 *
 * This is an ASYNCHRONOUS API: the request is enqueued and processed in
 * the background by a dedicated task, and the function returns immediately.
 * The caller must NOT assume the TTS has been broadcast when this returns;
 * only that the request was accepted. Failures to broadcast are logged.
 *
 * @param text      Short command text to speak (e.g., "提醒我休息")
 * @param priority  Metadata (0-3); this FIFO does not preempt a normal dialogue.
 * @return ESP_OK if the request was enqueued, error code otherwise
 */
esp_err_t xiaozhi_manager_speak(const char *text, int priority);

/* Paired by the single local PCM worker. No network operation or cloud-ready
 * requirement. Suppresses mic upload/cloud speaker admission until end(),
 * which restores upload only when the existing listening state permits it. */
esp_err_t xiaozhi_manager_local_audio_begin(void);
void xiaozhi_manager_local_audio_end(void);

/**
 * @brief  Send preset text for TTS playback (high-level API)
 *
 * High-level API for the "预设文本转化语音（主动注入机制）" requirement.
 * Sends a short preset text to the xiaozhi server which returns TTS audio.
 *
 * The text should be a short command/phrase (not a long sentence).
 * Examples: "提醒我休息", "久坐提醒", "专注提醒"
 *
 * Internally calls xiaozhi_manager_speak() with priority=2.
 *
 * @param text  Preset text for TTS playback
 * @return ESP_OK on success
 */
esp_err_t xiaozhi_manager_send_text(const char *text);

/**
 * @brief  Start listening for voice input
 *
 * Sends a "listen" message with state="start" to the server,
 * telling it to start processing incoming audio data.
 * Must be called after TTS ends to resume conversation.
 *
 * @param mode  Listening mode (ESP_XIAOZHI_CHAT_LISTENING_MODE_AUTO, etc.)
 * @return ESP_OK on success
 */
esp_err_t xiaozhi_manager_start_listening(int mode);

/**
 * @brief  Stop listening for voice input
 *
 * Sends a "listen" message with state="stop" to the server.
 *
 * @return ESP_OK on success
 */
esp_err_t xiaozhi_manager_stop_listening(void);

#ifdef __cplusplus
}
#endif
