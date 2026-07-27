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
 * @file rest_api.h
 * @brief REST API endpoints for FocusLamp_DEMO2 external control
 *
 * Registers HTTP REST endpoints on the existing websocket_manager HTTP server
 * (port 80) via ws_manager_server_register_uri().
 *
 * Endpoints (resource-style):
 *   GET  /api/status    → device status JSON
 *
 * Phase A: minimal status endpoint for connectivity verification
 * Phase B: device control endpoints (led/servo/display)
 * Phase C: status report endpoint (sensor data push)
 * Phase D: MCP tool wrapper endpoints
 * Phase E: audio control endpoints
 *
 * All JSON responses carry X-CRC16 header (CCITT) for integrity verification.
 */

/**
 * @brief Initialize the REST API module
 *
 * Registers all REST URI handlers on the websocket_manager HTTP server.
 * The websocket_manager must be initialized and server started before
 * calling this function.
 *
 * @return ESP_OK on success, error code on failure
 */
esp_err_t rest_api_init(void);

/**
 * @brief Deinitialize the REST API module
 *
 * Note: URI handlers cannot be unregistered from esp_http_server,
 * so this function only marks the module as deinitialized.
 */
void rest_api_deinit(void);

#ifdef __cplusplus
}
#endif
