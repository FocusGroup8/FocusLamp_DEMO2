#ifndef HEARTBEAT_SERVICE_H
#define HEARTBEAT_SERVICE_H

#include "esp_err.h"
#include "heartbeat_service_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#if (HEARTBEAT_SERVICE_ENABLE == 1)

/**
 * @brief Start heartbeat service for WebSocket server
 *
 * Periodically sends Ping frames to connected clients and
 * detects unresponsive clients via Pong timeout.
 *
 * @param ping_interval_sec  Ping interval in seconds (default 30)
 * @param timeout_sec        Client timeout in seconds (default 90)
 * @return ESP_OK on success
 */
esp_err_t heartbeat_service_start_server(int ping_interval_sec, int timeout_sec);

/**
 * @brief Stop heartbeat service
 *
 * @return ESP_OK on success
 */
esp_err_t heartbeat_service_stop(void);

/**
 * @brief Register callback for client timeout events
 *
 * Called when a client fails to respond to Ping within timeout.
 *
 * @param cb  Callback function (receives client_fd as int*)
 * @return ESP_OK on success
 */
esp_err_t heartbeat_service_register_timeout_cb(void (*cb)(int client_fd));

/**
 * @brief Notify heartbeat service of Pong received from client
 *
 * Call this when a Pong frame is received to reset the client's
 * timeout timer.
 *
 * @param client_fd  Client socket fd
 */
void heartbeat_service_pong_received(int client_fd);

#else /* HEARTBEAT_SERVICE_ENABLE == 0 */

static inline esp_err_t heartbeat_service_start_server(int ping_interval_sec, int timeout_sec)
{
    (void)ping_interval_sec;
    (void)timeout_sec;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t heartbeat_service_stop(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t heartbeat_service_register_timeout_cb(void (*cb)(int client_fd))
{
    (void)cb;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline void heartbeat_service_pong_received(int client_fd)
{
    (void)client_fd;
}

#endif /* HEARTBEAT_SERVICE_ENABLE */

#ifdef __cplusplus
}
#endif

#endif /* HEARTBEAT_SERVICE_H */
