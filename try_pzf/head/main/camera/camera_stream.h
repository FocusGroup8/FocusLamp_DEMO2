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

/* Diagnostic builds hold the producer at a runtime-selected 5, 8, 10 or 15 FPS,
 * fixed Q15, including through disconnects. Normal builds retain BBA/PI. */
#ifndef CAMERA_FIXED_DIAG
#define CAMERA_FIXED_DIAG 0
#endif
#if CAMERA_FIXED_DIAG != 0 && CAMERA_FIXED_DIAG != 1
#error "CAMERA_FIXED_DIAG must be 0 or 1"
#endif

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

/** Accept one background start/stop operation. ESP_OK means accepted.
 * ESP_ERR_INVALID_STATE means another operation or shutdown is in progress.
 * An optional completion runs on the control worker after the operation.
 * On admission failure the caller retains the context and no callback runs.
 * Completion and its result are also available through get_control_status(). */
esp_err_t camera_stream_request_control(bool start,
                                       void (*completion)(esp_err_t result, void *context), void *context);
void camera_stream_get_control_status(bool *busy, esp_err_t *last_result);

/**
 * @brief Check if streaming is active
 *
 * @return true if streaming task is running
 */
bool camera_stream_is_running(void);

/**
 * @brief Set JPEG encoding quality
 *
 * Q25 and Q35 select RAM-only clarity trials with congestion ceilings
 * of Q15/Q25/Q35. Other values restore the original adaptive policy.
 * Adaptive control may subsequently replace the applied value. With an FPS ceiling
 * of 1 or 2, low-load mode always applies Q15 (including at the encoder).
 * Use get_quality() for current control state and Stats actualQ for the last
 * logged frame's encoder input.
 * CAMERA_FIXED_DIAG accepts valid requests but always applies Q15; the
 * applied quality is returned by get_quality(), not the requested value.
 *
 * @param quality  JPEG quality (1-100)
 * @return ESP_OK on success
 */
esp_err_t camera_stream_set_quality(int quality);

/**
 * @brief Set the user frame-rate ceiling
 *
 * BBA may select a lower cadence; normal builds top out at 5 FPS. The separate
 * CAMERA_TRIAL_HIGH_FPS=8 build permits an 8 FPS HIGH level for comparison.
 * A ceiling of 1 or 2 also enables fixed-Q15 low-load mode. This is a runtime
 * setting, not a persistent firmware configuration.
 * CAMERA_FIXED_DIAG instead accepts 5, 8, 10 or 15 and uses that fixed producer
 * target without adaptive downgrade. This does not guarantee received FPS.
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
int camera_stream_get_trial_quality(void);
uint32_t camera_stream_last_jpeg_bytes(void);

/**
 * @brief Get current target FPS
 *
 * @return Current FPS value (1-30)
 */
int camera_stream_get_fps(void);
int camera_stream_get_fps_limit(void);
bool camera_stream_is_fixed_diag(void);
int camera_stream_get_max_fps(void);

#ifndef CAMERA_TRIAL_HIGH_FPS
#define CAMERA_TRIAL_HIGH_FPS 5
#endif

typedef struct {
    uint32_t seq, session, length;
    int64_t capture_us;
    uint32_t capture_cost_us, encode_us;
} camera_frame_info_t;

typedef struct {
    camera_frame_info_t frame;
    uint32_t ready_wait_us, lock_wait_us, network_us, wire_bytes, residence_us;
    const char *reason;  /* static string */
    int socket_errno;
} camera_delivery_t;

typedef struct {
    uint32_t enqueued, replaced, stale, stopped, resource_drops, oversize, metadata_drops;
    uint32_t sent, network_failures, preflight_drops;
    uint64_t jpeg_bytes;
    uint32_t producer_stack_min, sender_stack_min, producer_work_us;
    camera_frame_info_t produced;
    camera_delivery_t last_attempt, last_failure;
} camera_pipeline_stats_t;

void camera_stream_pipeline_stats(camera_pipeline_stats_t *out);

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
