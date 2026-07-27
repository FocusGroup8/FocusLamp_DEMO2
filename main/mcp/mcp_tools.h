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
 * Available tools (17 total):
 *   Camera: camera.start, camera.stop, camera.set_quality, camera.set_fps
 *   Display: display.on, display.off, display.set_brightness, display.show_camera
 *   LED: led.on, led.off, led.set_brightness, led.set_color_temp
 *   Eyes: eyes.set_expression, eyes.look_at, eyes.blink, eyes.get_expression
 *   Algorithm: algorithm.result
 *
 * ---------------------------------------------------------------------------
 * Architecture Notice (2026-07-19):
 * ---------------------------------------------------------------------------
 * This project (mipi_dsi) only exposes the MCP *interface layer* — protocol
 * parsing, tool metadata declaration (s_tool_meta), and the JSON-RPC 2.0
 * response builder. The concrete control logic (camera_stream_start/stop,
 * display brightness, etc.) is intentionally NOT implemented here.
 *
 * Cross-project collaboration:
 *   - wifi_test project integrates esp_xiaozhi MCP engine (esp_mcp_t)
 *   - wifi_test registers its own tools via esp_mcp_tool_create() and
 *     implements tool callbacks that connect to this project's /mcp endpoint
 *     over WebSocket to forward tool calls.
 *   - This project's /mcp endpoint responds to tools.list with the full tool
 *     metadata (interface contract), but tools.call returns "not_implemented"
 *     because the concrete control lives in wifi_test.
 *
 * To restore local control (e.g. for standalone testing), populate the
 * mcp_tools_callbacks_t fields in network_manager.c with real implementations.
 * ---------------------------------------------------------------------------
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

    /* LED control callbacks (4) */
    mcp_tool_cb_t led_on;             /*!< led.on: {brightness:int(0-100), color_temp:int(0-100)} */
    mcp_tool_cb_t led_off;            /*!< led.off: no args */
    mcp_tool_cb_t led_set_brightness; /*!< led.set_brightness: {brightness:int(0-100)} */
    mcp_tool_cb_t led_set_color_temp; /*!< led.set_color_temp: {color_temp:int(0-100)} */

    /* Expressive Eyes control callbacks (4) */
    mcp_tool_cb_t eyes_set_expression; /*!< eyes.set_expression: {expression:string} */
    mcp_tool_cb_t eyes_look_at;        /*!< eyes.look_at: {x:float(-1..1), y:float(-1..1)} */
    mcp_tool_cb_t eyes_blink;          /*!< eyes.blink: no args */
    mcp_tool_cb_t eyes_get_expression; /*!< eyes.get_expression: no args, returns {expression:string} */

    /* Algorithm result ingestion callback (1) */
    mcp_tool_cb_t algo_result; /*!< algorithm.result: {focus,emotion,fatigue,gesture,vlm_game_detector} */
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
