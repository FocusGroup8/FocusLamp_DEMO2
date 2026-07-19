/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file network_manager.h
 * @brief Network integration manager
 *
 * Coordinates WiFi connection, WebSocket server, MCP tools engine,
 * and camera streaming into a unified lifecycle.
 *
 * Startup sequence:
 *   1. WiFi connect (STA mode, blocks until IP obtained)
 *   2. WebSocket server start (port 80, paths: /camera, /mcp, /ws, /)
 *   3. MCP tools init + register callbacks
 *   4. Camera stream init (does not start capture yet)
 *   5. Register WebSocket DATA callback for /mcp message dispatch
 *
 * Runtime data flow:
 *   - /camera: camera_stream broadcasts JPEG binary frames
 *   - /mcp: JSON-RPC 2.0 requests -> mcp_tools -> tool callbacks
 */

/**
 * @brief Initialize network subsystem
 *
 * Must be called after system_manager_init() and after camera is initialized.
 * Starts WiFi, WebSocket server, MCP tools. Does NOT start camera streaming
 * (use network_manager_start_camera_stream() to begin streaming).
 *
 * @return ESP_OK on success
 */
esp_err_t network_manager_init(void);

/**
 * @brief Deinitialize network subsystem
 *
 * Stops camera streaming, WebSocket server, and disconnects WiFi.
 */
void network_manager_deinit(void);

/**
 * @brief Start camera streaming
 *
 * Begins JPEG frame broadcast to /camera WebSocket clients.
 * Requires network_manager_init() to have succeeded.
 *
 * @return ESP_OK on success
 */
esp_err_t network_manager_start_camera_stream(void);

/**
 * @brief Stop camera streaming
 *
 * @return ESP_OK on success
 */
esp_err_t network_manager_stop_camera_stream(void);

/**
 * @brief Get device IP address string
 *
 * @return IP address string (e.g. "192.168.1.100"), or empty string if not connected
 */
const char *network_manager_get_ip(void);

#ifdef __cplusplus
}
#endif
