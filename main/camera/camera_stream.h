/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "camera_controller.h"
#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file camera_stream.h
 * @brief Camera streaming task: captures frames, encodes JPEG, broadcasts via WebSocket
 *
 * Pipeline: OV5647 -> CSI -> ISP -> RGB565 -> JPEG encode -> WebSocket /camera broadcast
 *
 * The streaming task runs in its own FreeRTOS task and broadcasts JPEG binary
 * frames to all clients connected to the /camera WebSocket path.
 */

/**
 * @brief Camera stream configuration
 */
typedef struct {
    camera_handles_t *camera; /*!< Initialized camera handles (required) */
    int default_quality;      /*!< Default JPEG quality (1-100, default 80) */
    int default_fps;          /*!< Default target FPS (1-30, default 15) */
    int task_stack_size;      /*!< Streaming task stack size (default 8192) */
    int task_priority;        /*!< Streaming task priority (default 5) */
} camera_stream_config_t;

/**
 * @brief Initialize camera stream module
 *
 * Stores configuration but does not start streaming yet.
 *
 * @param config  Stream configuration
 * @return ESP_OK on success
 */
esp_err_t camera_stream_init(const camera_stream_config_t *config);

/**
 * @brief Deinitialize camera stream module
 *
 * Stops streaming if active and releases resources.
 */
void camera_stream_deinit(void);

/**
 * @brief Start camera streaming
 *
 * Starts the camera pipeline and launches the streaming task.
 * JPEG frames will be broadcast to all /camera WebSocket clients.
 *
 * @return ESP_OK on success
 */
esp_err_t camera_stream_start(void);

/**
 * @brief Stop camera streaming
 *
 * Stops the streaming task and camera pipeline.
 *
 * @return ESP_OK on success
 */
esp_err_t camera_stream_stop(void);

/**
 * @brief Check if streaming is active
 *
 * @return true if streaming task is running
 */
bool camera_stream_is_running(void);

/**
 * @brief Set JPEG encoding quality
 *
 * @param quality  JPEG quality (1-100)
 * @return ESP_OK on success
 */
esp_err_t camera_stream_set_quality(int quality);

/**
 * @brief Set target frame rate
 *
 * @param fps  Target FPS (1-30)
 * @return ESP_OK on success
 */
esp_err_t camera_stream_set_fps(int fps);

/**
 * @brief Get current JPEG quality
 *
 * @return Current quality value (1-100)
 */
int camera_stream_get_quality(void);

/**
 * @brief Get current target FPS
 *
 * @return Current FPS value (1-30)
 */
int camera_stream_get_fps(void);

/**
 * @brief Get streaming statistics
 *
 * @param frames_sent       Output: total frames sent
 * @param frames_failed     Output: total frames failed to send
 * @param avg_frame_size    Output: average frame size in bytes
 * @param avg_fps_actual    Output: actual achieved FPS
 */
void camera_stream_get_stats(uint32_t *frames_sent, uint32_t *frames_failed, uint32_t *avg_frame_size,
                             float *avg_fps_actual);

#ifdef __cplusplus
}
#endif
