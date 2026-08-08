/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

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

/**
 * @brief Algorithm result state (latest values received via algorithm.result tool)
 *
 * Populated by the MCP `algorithm.result` tool callback when main-client pushes
 * algorithm detection results to the /algo WebSocket endpoint. Fields are
 * updated under a mutex; `last_update_us` is zero until first update.
 *
 * NOTE: Execution logic (LED/display/eyes actuation based on these values) is
 * intentionally NOT implemented in this revision — consumers may read this
 * state and decide actions locally.
 */
typedef struct {
    char emotion[32];            /*!< Emotion type string, e.g. "Happiness" */
    int fatigue;                 /*!< Fatigue rating integer */
    char focus_level_name[32];   /*!< Focus level name, e.g. "Medium" */
    char engage_level_name[32];  /*!< Engagement level name, e.g. "Engaged" */
    float focus_score;           /*!< Focus score 0..1 */
    char gesture[32];            /*!< Gesture name, e.g. "Thumb_Up" */
    char vlm_judgment[16];       /*!< VLM game detector judgment: 是/否/不确定/等待检测 */
    char vlm_trigger_source[16]; /*!< VLM trigger source: None/手机/电脑 */
    char vlm_reason[128];        /*!< VLM detection reason text */
    bool present;                /*!< User presence (true=present, derived from face_position.valid) */
    float presence_duration_s;   /*!< Presence duration in seconds (0.0 when absent) */
    int64_t last_update_us;      /*!< esp_timer_get_time() of last update, 0 if never */
} algo_result_state_t;

/**
 * @brief Get pointer to the latest algorithm result state (read-only snapshot)
 *
 * The returned pointer is to a static internal buffer; caller must not free it
 * or write through it. Fields are updated atomically under an internal mutex.
 *
 * @return Pointer to internal algo_result_state_t (always non-NULL)
 */
const algo_result_state_t *network_manager_get_algo_result_state(void);

#ifdef __cplusplus
}
#endif
