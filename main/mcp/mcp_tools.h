/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file mcp_tools.h
 * @brief Lightweight MCP (Model Context Protocol) tools engine
 *
 * Implements JSON-RPC 2.0 message parsing and dispatches to registered
 * tool callbacks. Designed for the ESP32-P4 WebSocket camera stream project.
 *
 * Protocol version: 2025-11-25
 * Transport: WebSocket text frames on /mcp path
 *
 * Supported methods:
 *   - initialize
 *   - tools.list
 *   - tools.call
 *   - ping
 *
 * Available tools (8 total):
 *   Camera: camera.start, camera.stop, camera.set_quality, camera.set_fps
 *   Display: display.on, display.off, display.set_brightness, display.show_camera
 */

/**
 * @brief MCP tool callback function type
 *
 * @param args_json  Arguments as cJSON object (may be NULL)
 * @param response_buf  Buffer to write response JSON string
 * @param response_buf_size  Response buffer capacity
 * @return ESP_OK on success, error code on failure
 */
typedef esp_err_t (*mcp_tool_cb_t)(const void *args_json, char *response_buf, int response_buf_size);

/**
 * @brief MCP tools callbacks registry
 *
 * Each callback corresponds to one MCP tool. Set to NULL to mark as "not implemented".
 */
typedef struct {
    /* Camera control callbacks (4) */
    mcp_tool_cb_t camera_start;       /*!< camera.start: no args, returns {"ok":true} */
    mcp_tool_cb_t camera_stop;        /*!< camera.stop: no args, returns {"ok":true} */
    mcp_tool_cb_t camera_set_quality; /*!< camera.set_quality: {quality:int(1-100)} */
    mcp_tool_cb_t camera_set_fps;     /*!< camera.set_fps: {fps:int(1-30)} */

    /* Display control callbacks (4) */
    mcp_tool_cb_t display_on;             /*!< display.on: no args */
    mcp_tool_cb_t display_off;            /*!< display.off: no args */
    mcp_tool_cb_t display_set_brightness; /*!< display.set_brightness: {level:int(0-100)} */
    mcp_tool_cb_t display_show_camera;    /*!< display.show_camera: {enable:bool} */
} mcp_tools_callbacks_t;

/**
 * @brief Initialize MCP tools engine
 *
 * @return ESP_OK on success
 */
esp_err_t mcp_tools_init(void);

/**
 * @brief Deinitialize MCP tools engine
 */
void mcp_tools_deinit(void);

/**
 * @brief Register tool callbacks
 *
 * @param callbacks  Callback structure (copied internally)
 * @return ESP_OK on success
 */
esp_err_t mcp_tools_register_callbacks(const mcp_tools_callbacks_t *callbacks);

/**
 * @brief Handle incoming JSON-RPC message and produce response
 *
 * Parses the JSON-RPC 2.0 request, dispatches to the appropriate tool,
 * and writes the response into the response buffer.
 *
 * @param request_json  Incoming JSON-RPC request string (null-terminated)
 * @param response_buf  Buffer to write response JSON (null-terminated)
 * @param response_buf_size  Response buffer capacity
 * @return ESP_OK on success, error code on parse/dispatch failure
 */
esp_err_t mcp_tools_handle_message(const char *request_json, char *response_buf, int response_buf_size);

#ifdef __cplusplus
}
#endif
