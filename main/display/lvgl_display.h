/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file lvgl_display.h
 * @brief LVGL display module for ESP32-P4 MIPI DSI LCD
 *
 * Integrates LVGL 9.x with MIPI DSI display via esp_lvgl_port.
 * Supports avoid_tearing mode (uses DPI frame buffers as LVGL draw buffers).
 */

/**
 * @brief LVGL display configuration
 */
typedef struct {
    esp_lcd_panel_handle_t panel_handle; /*!< DPI panel handle from board_init_lcd() */
    esp_lcd_touch_handle_t touch_handle; /*!< Touch handle (can be NULL) */
    uint32_t hres;                       /*!< Horizontal resolution */
    uint32_t vres;                       /*!< Vertical resolution */
    bool avoid_tearing;                  /*!< Use DPI frame buffers as LVGL buffers (requires double-buffer) */
} lvgl_display_cfg_t;

/**
 * @brief LVGL display context (opaque handles)
 */
typedef struct {
    lv_display_t *disp;           /*!< LVGL display handle */
    lv_indev_t *touch_indev;      /*!< LVGL touch input device (NULL if no touch) */
    esp_lcd_panel_handle_t panel; /*!< DPI panel handle */
} lvgl_display_t;

/**
 * @brief Initialize LVGL display system
 *
 * Initializes esp_lvgl_port, creates DSI display, and optionally adds touch input.
 * Starts the LVGL task (internal to esp_lvgl_port).
 *
 * @param cfg   Display configuration
 * @param[out] ctx  LVGL display context (populated on success)
 * @return ESP_OK on success
 */
esp_err_t lvgl_display_init(const lvgl_display_cfg_t *cfg, lvgl_display_t *ctx);

/**
 * @brief Create a simple demo UI for verification
 *
 * Shows "ESP32-P4 LVGL" title and a button on screen.
 * Must be called after lvgl_display_init().
 *
 * @param ctx  LVGL display context
 */
void lvgl_display_demo_ui(lvgl_display_t *ctx);

/**
 * @brief Deinitialize LVGL display system
 *
 * Removes touch, display, and deinitializes esp_lvgl_port.
 *
 * @param ctx  LVGL display context
 * @return ESP_OK on success
 */
esp_err_t lvgl_display_deinit(lvgl_display_t *ctx);

#ifdef __cplusplus
}
#endif
