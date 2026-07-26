/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "esp_mcp_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file mipi_dsi_bridge.h
 * @brief HTTP bridge for controlling mipi_dsi device from wifi_test
 *
 * Registers MCP tools that forward control commands to mipi_dsi's
 * REST API endpoints via HTTP POST requests.
 *
 * Tool → REST API mapping:
 *   self.mipi_dsi.camera.start       → POST /api/camera/start
 *   self.mipi_dsi.camera.stop        → POST /api/camera/stop
 *   self.mipi_dsi.camera.set_quality → POST /api/camera/quality
 *   self.mipi_dsi.camera.set_fps     → POST /api/camera/fps
 *   self.mipi_dsi.display.on         → POST /api/display/on
 *   self.mipi_dsi.display.off        → POST /api/display/off
 *   self.mipi_dsi.display.set_brightness → POST /api/display/brightness
 *   self.mipi_dsi.display.show_camera    → POST /api/display/camera_preview
 *   self.mipi_dsi.led.on             → POST /api/led/on
 *   self.mipi_dsi.led.off            → POST /api/led/off
 *   self.mipi_dsi.led.set_brightness → POST /api/led/brightness
 *   self.mipi_dsi.led.set_color_temp → POST /api/led/color_temp
 */

/**
 * @brief Initialize the mipi_dsi bridge
 *
 * Reads mipi_dsi device IP from configuration and prepares
 * the HTTP client for bridging requests.
 *
 * @return ESP_OK on success
 */
esp_err_t mipi_dsi_bridge_init(void);

/**
 * @brief Deinitialize the mipi_dsi bridge
 */
void mipi_dsi_bridge_deinit(void);

/**
 * @brief Register MCP tools for mipi_dsi control
 *
 * Registers 8 MCP tools that forward commands to mipi_dsi
 * via HTTP REST API. Must be called after mipi_dsi_bridge_init()
 * and with a valid MCP engine instance.
 *
 * @param mcp  MCP engine instance (from xiaozhi_manager_get_mcp_engine())
 * @return ESP_OK on success
 */
esp_err_t mipi_dsi_bridge_register_mcp_tools(esp_mcp_t *mcp);

/**
 * @brief Check if mipi_dsi device is reachable
 *
 * Sends a GET /api/status request to verify connectivity.
 *
 * @return ESP_OK if device is reachable, error code otherwise
 */
esp_err_t mipi_dsi_bridge_ping(void);

#ifdef __cplusplus
}
#endif
