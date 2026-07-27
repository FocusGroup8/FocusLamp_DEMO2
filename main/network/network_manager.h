/*
 * network_manager.h - Network subsystem manager for FocusLamp (dowm)
 *
 * Single entry point that brings up the full communication stack in the
 * same order and with the same structure as Project 1 (FocusLamp_DEMO2):
 *
 *   1. WiFi Manager            (wifi_manager)         -> IP connectivity
 *   2. WebSocket Server        (websocket_manager)    -> /ws + /mcp endpoints
 *   3. MCP Handler             (app_mcp_handler)      -> JSON-RPC 2.0 tool dispatch on /mcp
 *   4. Connection Manager      (connection_manager)   -> health monitoring + auto-recovery
 *   5. Heartbeat Service       (heartbeat_service)    -> WebSocket Ping/Pong liveness
 *   6. Message Queue           (message_queue)        -> reliable delivery queue
 *
 * Differences from Project 1:
 *   - No camera stream (dowm has no camera hardware)
 *   - No REST API registration here (dowm uses /ws echo + /mcp JSON-RPC only;
 *     REST endpoints may be added in a later phase if required)
 *   - MCP dispatch uses dowm's app_mcp_handler instead of Project 1's mcp_tools
 */

#pragma once
#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the full network/communication subsystem
 *
 * Sequence (mirrors Project 1 / FocusLamp_DEMO2):
 *   1. Register WiFi event callbacks (BEFORE init to catch GOT_IP)
 *   2. wifi_manager_init() - blocks until WiFi connects (up to 30s timeout)
 *   3. ws_manager_init() + ws_manager_server_start() - WebSocket on port 80
 *   4. app_mcp_handler_init() - registers /mcp DATA handler on ws_manager
 *   5. connection_manager_init() + connection_manager_start() - health monitor
 *   6. heartbeat_service_start_server(30, 90) - Ping/Pong liveness
 *   7. message_queue_init(16, 3) - reliable delivery queue
 *
 * Must be called AFTER all hardware services (led_service, lcd_module, etc.)
 * are initialized, because app_mcp_handler dispatches to hardware control APIs.
 *
 * @return ESP_OK on success, error code on failure (non-fatal in app_init)
 */
esp_err_t network_manager_init(void);

/**
 * @brief Deinitialize the network subsystem
 *
 * Stops services in reverse initialization order.
 */
void network_manager_deinit(void);

/**
 * @brief Get the device IP address string
 *
 * @return IP string (e.g. "192.168.1.100"), or empty string if not connected
 */
const char *network_manager_get_ip(void);

/**
 * @brief Check if the network subsystem is fully initialized
 *
 * @return true if network_manager_init() succeeded
 */
bool network_manager_is_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* NETWORK_MANAGER_H */
