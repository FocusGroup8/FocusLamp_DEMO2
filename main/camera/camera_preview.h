/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "camera_controller.h"
#include "driver/ppa.h"
#include "esp_err.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file camera_preview.h
 * @brief Camera preview module using PPA hardware for scaling
 *
 * Takes camera RGB565 frames (800x640) and uses PPA SRM to:
 *   1. Copy frame to a CPU-owned input buffer (isolates ISP DMA from PPA DMA)
 *   2. Scale down to 400x320 (0.5x, no crop)
 *
 * Output is RGB565 (same color mode — PPA SRM does not support
 * cross-color-space conversion). Uses LV_COLOR_FORMAT_RGB565 canvas
 * matching the DPI panel (BSP_LCD_BIGENDIAN=0, little-endian).
 */

/**
 * @brief Camera preview context
 */
typedef struct {
    ppa_client_handle_t ppa_client; /*!< PPA SRM client handle */
    uint8_t *out_buf;               /*!< PPA output buffer (RGB565 400x320, in PSRAM) */
    uint32_t out_buf_size;          /*!< PPA output buffer size in bytes */
    uint8_t *ppa_input_buf;         /*!< PPA input buffer (copy of frame, isolates ISP DMA) */
    uint32_t ppa_input_buf_size;    /*!< PPA input buffer size in bytes */
    bool is_initialized;            /*!< Initialization state */
} camera_preview_t;

/**
 * @brief Initialize camera preview module
 *
 * Registers PPA SRM client and allocates output buffer in PSRAM.
 *
 * @param preview  Camera preview context (zeroed before use)
 * @return ESP_OK on success
 */
esp_err_t camera_preview_init(camera_preview_t *preview);

/**
 * @brief Process one camera frame through PPA
 *
 * Takes the current frame in camera_handles->frame_buffer (RGB565 800x640),
 * center-crops to 640x640, scales to 480x480 (RGB565 output).
 * Result is stored in preview->out_buf.
 *
 * @param handles   Camera handles (must have a captured frame)
 * @param preview   Camera preview context (must be initialized)
 * @return ESP_OK on success
 */
esp_err_t camera_preview_process_frame(const camera_handles_t *handles, camera_preview_t *preview);

/**
 * @brief Get the output buffer pointer
 *
 * @param preview  Camera preview context
 * @return Pointer to RGB565 480x480 buffer, or NULL if not initialized
 */
uint8_t *camera_preview_get_buffer(const camera_preview_t *preview);

/**
 * @brief Deinitialize camera preview module
 *
 * Unregisters PPA client and frees output buffer.
 *
 * @param preview  Camera preview context
 * @return ESP_OK on success
 */
esp_err_t camera_preview_deinit(camera_preview_t *preview);

#ifdef __cplusplus
}
#endif
