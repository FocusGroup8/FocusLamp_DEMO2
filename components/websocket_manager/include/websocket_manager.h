#ifndef WEBSOCKET_MANAGER_H
#define WEBSOCKET_MANAGER_H

#include "websocket_manager_config.h"
#include "websocket_manager_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#if (WS_MANAGER_ENABLE == 1)

/* ======================== Common API ======================== */

/**
 * @brief Initialize WebSocket Manager
 *
 * @return ESP_OK on success, error code on failure
 */
esp_err_t ws_manager_init(void);

/**
 * @brief Deinitialize WebSocket Manager
 *
 * @return ESP_OK on success, error code on failure
 */
esp_err_t ws_manager_deinit(void);

/**
 * @brief Register event callback
 *
 * @param event  Event type to register for
 * @param cb     Callback function
 * @return ESP_OK on success, error code on failure
 */
esp_err_t ws_manager_register_handler(ws_manager_event_t event, ws_manager_cb_t cb);

/* ======================== Client API ======================== */

#if (WS_MANAGER_CLIENT_ENABLE == 1)

/**
 * @brief Start WebSocket client and connect to server
 *
 * @param config  Client configuration (URI, subprotocol, etc.)
 * @return ESP_OK on success, error code on failure
 */
esp_err_t ws_manager_client_start(const ws_manager_client_config_t *config);

/**
 * @brief Stop WebSocket client and disconnect
 *
 * @return ESP_OK on success, error code on failure
 */
esp_err_t ws_manager_client_stop(void);

/**
 * @brief Check if client is connected
 *
 * @return true if connected, false otherwise
 */
bool ws_manager_client_is_connected(void);

/**
 * @brief Send text data via client
 *
 * @param data  Data to send
 * @param len   Data length
 * @param timeout_ms  Timeout in milliseconds (-1 for blocking)
 * @return Number of bytes sent, or -1 on error
 */
int ws_manager_client_send_text(const char *data, int len, int timeout_ms);

/**
 * @brief Send binary data via client
 *
 * @param data  Data to send
 * @param len   Data length
 * @param timeout_ms  Timeout in milliseconds (-1 for blocking)
 * @return Number of bytes sent, or -1 on error
 */
int ws_manager_client_send_binary(const char *data, int len, int timeout_ms);

/**
 * @brief Get client connection info
 *
 * @param info  Pointer to ws_manager_conn_info_t to fill
 * @return ESP_OK on success, error code on failure
 */
esp_err_t ws_manager_client_get_info(ws_manager_conn_info_t *info);

#endif /* WS_MANAGER_CLIENT_ENABLE */

/* ======================== Server API ======================== */

#if (WS_MANAGER_SERVER_ENABLE == 1)

/**
 * @brief Start WebSocket server
 *
 * @return ESP_OK on success, error code on failure
 */
esp_err_t ws_manager_server_start(void);

/**
 * @brief Stop WebSocket server
 *
 * @return ESP_OK on success, error code on failure
 */
esp_err_t ws_manager_server_stop(void);

/**
 * @brief Check if server is running
 *
 * @return true if running, false otherwise
 */
bool ws_manager_server_is_running(void);

/**
 * @brief Send text data to a specific client
 *
 * @param client_fd  Client socket fd
 * @param data       Data to send
 * @param len        Data length
 * @return ESP_OK on success, error code on failure
 */
esp_err_t ws_manager_server_send_text(int client_fd, const char *data, int len);

/**
 * @brief Send binary data to a specific client
 *
 * @param client_fd  Client socket fd
 * @param data       Data to send
 * @param len        Data length
 * @return ESP_OK on success, error code on failure
 */
esp_err_t ws_manager_server_send_binary(int client_fd, const char *data, int len);

/**
 * @brief Broadcast text data to all connected clients
 *
 * @param data  Data to send
 * @param len   Data length
 * @return ESP_OK on success, error code on failure
 */
esp_err_t ws_manager_server_broadcast_text(const char *data, int len);

/**
 * @brief Broadcast binary data to all connected clients
 *
 * @param data  Data to send
 * @param len   Data length
 * @return ESP_OK on success, error code on failure
 */
esp_err_t ws_manager_server_broadcast_binary(const char *data, int len);

/**
 * @brief Get number of connected clients
 *
 * @return Number of connected clients
 */
int ws_manager_server_get_client_count(void);

#endif /* WS_MANAGER_SERVER_ENABLE */

#else /* WS_MANAGER_ENABLE == 0 */

/* Stubs when disabled */
static inline esp_err_t ws_manager_init(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t ws_manager_deinit(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t ws_manager_register_handler(ws_manager_event_t event, ws_manager_cb_t cb) { (void)event; (void)cb; return ESP_ERR_NOT_SUPPORTED; }

#if (WS_MANAGER_CLIENT_ENABLE == 1)
// Client stubs would conflict with disabled macro, so not needed
#endif

#endif /* WS_MANAGER_ENABLE */

#ifdef __cplusplus
}
#endif

#endif /* WEBSOCKET_MANAGER_H */
