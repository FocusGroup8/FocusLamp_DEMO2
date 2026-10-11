#ifndef XIAOZHI_MANAGER_TYPES_H
#define XIAOZHI_MANAGER_TYPES_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Xiaozhi Manager state enumeration
 */
typedef enum {
    XIAOZHI_MANAGER_STATE_IDLE,         /*!< Idle, not connected */
    XIAOZHI_MANAGER_STATE_CONNECTING,   /*!< Connecting to server */
    XIAOZHI_MANAGER_STATE_CONNECTED,    /*!< Connected to server */
    XIAOZHI_MANAGER_STATE_LISTENING,    /*!< Listening for user speech */
    XIAOZHI_MANAGER_STATE_SPEAKING,     /*!< Speaking (TTS playback) */
    XIAOZHI_MANAGER_STATE_ERROR,        /*!< Error state */
} xiaozhi_manager_state_t;

/**
 * @brief Xiaozhi Manager event types
 */
typedef enum {
    XIAOZHI_MANAGER_EVENT_CONNECTED,            /*!< Connected to server */
    XIAOZHI_MANAGER_EVENT_DISCONNECTED,         /*!< Disconnected from server */
    XIAOZHI_MANAGER_EVENT_AUDIO_CHANNEL_OPENED, /*!< Audio channel opened */
    XIAOZHI_MANAGER_EVENT_AUDIO_CHANNEL_CLOSED, /*!< Audio channel closed */
    XIAOZHI_MANAGER_EVENT_TTS_START,            /*!< TTS playback started */
    XIAOZHI_MANAGER_EVENT_TTS_STOP,             /*!< TTS playback stopped */
    XIAOZHI_MANAGER_EVENT_TTS_SENTENCE,         /*!< TTS sentence text received */
    XIAOZHI_MANAGER_EVENT_CHAT_TEXT,            /*!< Chat text received (STT/TTS) */
    XIAOZHI_MANAGER_EVENT_CHAT_ERROR,           /*!< Chat error occurred */
    XIAOZHI_MANAGER_EVENT_SYSTEM_CMD,           /*!< System command from server */
} xiaozhi_manager_event_t;

/**
 * @brief Xiaozhi Manager event callback type
 *
 * @param event  Event type
 * @param data   Event data (context-dependent, may be NULL)
 */
typedef void (*xiaozhi_manager_event_cb_t)(xiaozhi_manager_event_t event, void *data);

#ifdef __cplusplus
}
#endif

#endif /* XIAOZHI_MANAGER_TYPES_H */
