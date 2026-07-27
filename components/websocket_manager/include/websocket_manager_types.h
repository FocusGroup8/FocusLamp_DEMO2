#ifndef WEBSOCKET_MANAGER_TYPES_H
#define WEBSOCKET_MANAGER_TYPES_H

#include "esp_err.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WebSocket Manager event types
 */
typedef enum {
    WS_MANAGER_EVENT_CONNECTED,         /*!< WebSocket connected */
    WS_MANAGER_EVENT_DISCONNECTED,      /*!< WebSocket disconnected */
    WS_MANAGER_EVENT_DATA,              /*!< Data received */
    WS_MANAGER_EVENT_ERROR,             /*!< Error occurred */
    WS_MANAGER_EVENT_SERVER_CONNECT,    /*!< New client connected to server */
    WS_MANAGER_EVENT_SERVER_DISCONNECT, /*!< Client disconnected from server */
    WS_MANAGER_EVENT_CLIENT_TIMEOUT,    /*!< Server client heartbeat timeout */
} ws_manager_event_t;

/**
 * @brief WebSocket data types
 */
typedef enum {
    WS_DATA_TYPE_TEXT,   /*!< Text data (UTF-8) */
    WS_DATA_TYPE_BINARY, /*!< Binary data */
} ws_data_type_t;

/**
 * @brief WebSocket connection info
 */
typedef struct {
    bool is_connected; /*!< Connection status */
    char uri[128];     /*!< Connected URI (client) */
    int fd;            /*!< Socket fd (server client) */
} ws_manager_conn_info_t;

/**
 * @brief WebSocket data event payload
 */
typedef struct {
    ws_data_type_t type; /*!< Data type */
    const char *data;    /*!< Data pointer (do NOT free) */
    int data_len;        /*!< Data length */
    int client_fd;       /*!< Client fd (server mode, -1 for client mode) */
    const char *uri;     /*!< Request URI (server mode, e.g. "/camera", "/mcp"; NULL for client mode) */
} ws_manager_data_t;

/**
 * @brief WebSocket client configuration (runtime, API-set)
 */
typedef struct {
    const char *uri;         /*!< WebSocket URI (ws:// or wss://) */
    const char *subprotocol; /*!< WebSocket subprotocol */
    const char *user_agent;  /*!< User agent string */
    const char *headers;     /*!< Additional headers (each terminated with \r\n) */
    const char *cert_pem;    /*!< Server certificate for TLS (PEM format, NULL for no verify) */
} ws_manager_client_config_t;

/**
 * @brief WebSocket Manager event callback type
 *
 * @param event  Event type
 * @param data   Event data (type-dependent, may be NULL)
 */
typedef void (*ws_manager_cb_t)(ws_manager_event_t event, void *data);

/**
 * @brief WebSocket server send statistics for congestion detection
 *
 * Provides signals for adaptive streaming algorithms to detect network
 * congestion and adjust frame rate / quality accordingly.
 */
typedef struct {
    uint32_t total_sent;           /*!< Total frames successfully sent */
    uint32_t total_failed;         /*!< Total frames failed to send */
    uint32_t consecutive_failures; /*!< Current consecutive failure count */
    uint32_t pool_exhausted;       /*!< Frame buffer pool exhausted count (backpressure) */
    int last_error;                /*!< Last error code (errno value: 11=EAGAIN, 104=ECONNRESET) */
    int64_t last_send_duration_us; /*!< Last send duration in microseconds (queue to completion) */
    int64_t avg_send_duration_us;  /*!< EWMA of send duration (α=0.2) */
} ws_send_stats_t;

#ifdef __cplusplus
}
#endif

#endif /* WEBSOCKET_MANAGER_TYPES_H */
