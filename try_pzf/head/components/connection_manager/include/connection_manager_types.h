#ifndef CONNECTION_MANAGER_TYPES_H
#define CONNECTION_MANAGER_TYPES_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Connection layer identifier
 */
typedef enum {
    CONN_MGR_LAYER_WIFI,      /*!< WiFi layer */
    CONN_MGR_LAYER_WS_CLIENT, /*!< WebSocket client layer */
    CONN_MGR_LAYER_WS_SERVER, /*!< WebSocket server layer */
    CONN_MGR_LAYER_MAX
} conn_mgr_layer_t;

/**
 * @brief Connection state for each layer
 */
typedef enum {
    CONN_MGR_STATE_IDLE,       /*!< Not initialized */
    CONN_MGR_STATE_CONNECTING, /*!< Connecting/starting */
    CONN_MGR_STATE_CONNECTED,  /*!< Connected/running */
    CONN_MGR_STATE_DEGRADED,   /*!< Degraded (reconnecting, partial failure) */
    CONN_MGR_STATE_FAILED,     /*!< Failed (retries exhausted) */
    CONN_MGR_STATE_RECOVERING, /*!< Recovery in progress */
} conn_mgr_state_t;

/**
 * @brief Connection Manager event types
 */
typedef enum {
    CONN_MGR_EVENT_WIFI_CONNECTED,         /*!< WiFi connected */
    CONN_MGR_EVENT_WIFI_DISCONNECTED,      /*!< WiFi disconnected */
    CONN_MGR_EVENT_WIFI_RECOVERING,        /*!< WiFi recovery in progress */
    CONN_MGR_EVENT_WIFI_FAILED,            /*!< WiFi failed (retries exhausted) */
    CONN_MGR_EVENT_WS_CLIENT_CONNECTED,    /*!< WS client connected */
    CONN_MGR_EVENT_WS_CLIENT_DISCONNECTED, /*!< WS client disconnected */
    CONN_MGR_EVENT_WS_CLIENT_RECOVERING,   /*!< WS client recovery in progress */
    CONN_MGR_EVENT_WS_SERVER_STARTED,      /*!< WS server started */
    CONN_MGR_EVENT_WS_SERVER_STOPPED,      /*!< WS server stopped */
    CONN_MGR_EVENT_WS_SERVER_RECOVERING,   /*!< WS server recovery in progress */
    CONN_MGR_EVENT_WS_CLIENT_TIMEOUT,      /*!< WS client heartbeat timeout */
    CONN_MGR_EVENT_HOSTED_TRANSPORT_DOWN,  /*!< ESP-Hosted transport down */
    CONN_MGR_EVENT_MAX
} conn_mgr_event_t;

/**
 * @brief Connection statistics
 */
typedef struct {
    conn_mgr_state_t layer_state[CONN_MGR_LAYER_MAX]; /*!< State per layer */
    uint32_t wifi_reconnect_count;                    /*!< WiFi reconnect attempts */
    uint32_t ws_client_reconnect_count;               /*!< WS client reconnect attempts */
    uint32_t ws_server_restart_count;                 /*!< WS server restart count */
    uint32_t hosted_transport_failures;               /*!< ESP-Hosted transport failures */
    int64_t last_wifi_connect_ms;                     /*!< Last WiFi connect timestamp */
    int64_t last_ws_connect_ms;                       /*!< Last WS connect timestamp */
} conn_mgr_stats_t;

/**
 * @brief Connection Manager configuration
 */
typedef struct {
    int wifi_init_timeout_ms;        /*!< WiFi init timeout (default 30000) */
    int wifi_reconnect_max_delay_ms; /*!< WiFi reconnect max delay (default 60000) */
    int ws_reconnect_max_delay_ms;   /*!< WS reconnect max delay (default 60000) */
    int monitor_interval_ms;         /*!< Health monitor interval (default 5000) */
} conn_mgr_config_t;

/**
 * @brief Connection Manager event callback type
 *
 * @param event  Event type
 * @param data   Event data (may be NULL)
 */
typedef void (*conn_mgr_cb_t)(conn_mgr_event_t event, void *data);

#ifdef __cplusplus
}
#endif

#endif /* CONNECTION_MANAGER_TYPES_H */
