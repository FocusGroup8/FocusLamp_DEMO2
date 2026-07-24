/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/**
 * @file display_system.h
 * @brief Display system top-level interface for ESP32-P4 MIPI DSI LCD subsystem
 *
 * This module provides a unified interface for the display subsystem, including:
 * - MIPI DSI LCD driver (KD034WXFID001 480x480 panel with ST7701S driver IC)
 * - Touch controller (GT911 via I2C)
 * - GUI drawing library with double-buffer support
 * - Gesture recognition and touch interaction demos
 *
 * The display system can operate independently without audio subsystem,
 * supporting the mutually exclusive operation mode with audio system.
 */

#include "esp_err.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Display system operation modes
 */
typedef enum {
    DISPLAY_MODE_TOUCH_GAME,          /*!< Touch game demo (gesture-controlled ball) */
    DISPLAY_MODE_GESTURE_RECOGNITION, /*!< Gesture recognition demo */
    DISPLAY_MODE_TOUCH_GUI,           /*!< Touch GUI demo (button test) */
    DISPLAY_MODE_DATA_COLLECTOR,      /*!< Gesture data collector for calibration */
    DISPLAY_MODE_LVGL,                /*!< LVGL framework mode (esp_lvgl_port) */
} display_mode_t;

/**
 * @brief Display system state
 */
typedef enum {
    DISPLAY_STATE_IDLE,    /*!< System initialized but not running */
    DISPLAY_STATE_RUNNING, /*!< Demo is running */
    DISPLAY_STATE_PAUSED,  /*!< Demo paused (if supported) */
    DISPLAY_STATE_ERROR,   /*!< System error state */
} display_state_t;

/**
 * @brief Display system configuration
 */
typedef struct {
    display_mode_t mode;       /*!< Demo mode to run */
    bool enable_touch;         /*!< Enable touch controller (default: true) */
    bool enable_double_buffer; /*!< Enable double-buffer mode (default: true) */
    bool enable_ppa_accel;     /*!< Enable PPA hardware acceleration (default: true) */
} display_config_t;

/**
 * @brief Display system handles (opaque to caller)
 */
typedef struct {
    void *panel_handle;                  /*!< LCD panel handle (opaque) */
    void *gui_handle;                    /*!< GUI context handle (opaque) */
    esp_lcd_touch_handle_t touch_handle; /*!< Touch controller handle */
    void *lvgl_ctx;                      /*!< LVGL display context (opaque, used in LVGL mode) */
} display_handles_t;

/**
 * @brief Initialize the display system
 *
 * Initializes all display subsystems:
 * - MIPI DSI PHY power (LDO)
 * - Backlight GPIO
 * - MIPI DSI bus and DBI panel IO
 * - ST7701S LCD driver with KD034WXFID001 init sequence
 * - DPI panel timing (480x480 @ 60Hz)
 * - Double-buffer frame buffers
 * - PPA hardware accelerator
 * - GT911 touch controller (if enabled)
 *
 * @param config  Display system configuration (NULL for defaults)
 * @param[out] handles  Display handles (can be NULL if not needed)
 * @return ESP_OK on success
 */
esp_err_t display_system_init(const display_config_t *config, display_handles_t *handles);

/**
 * @brief Start display system in the configured demo mode
 *
 * Runs the selected demo according to configured mode:
 * - TOUCH_GAME: launches gesture-controlled ball game
 * - GESTURE_RECOGNITION: runs gesture recognition demo
 * - TOUCH_GUI: runs button test GUI demo
 * - DATA_COLLECTOR: runs gesture data collection for calibration
 *
 * @param handles  Display handles from init
 * @return ESP_OK on success
 */
esp_err_t display_system_start(const display_handles_t *handles);

/**
 * @brief Stop display system demo
 *
 * Stops the currently running demo and clears the screen.
 * Does not deinitialize the system (can be restarted with different demo).
 *
 * @param handles  Display handles
 * @return ESP_OK on success
 */
esp_err_t display_system_stop(const display_handles_t *handles);

/**
 * @brief Get current display system state
 *
 * @return Current state
 */
display_state_t display_system_get_state(void);

/**
 * @brief Show black screen (display "off" visual effect)
 *
 * Clears the active screen and fills it with black.
 * Only works in LVGL mode. Can be restored with display_system_show_ui().
 *
 * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED if not in LVGL mode
 */
esp_err_t display_system_show_black_screen(void);

/**
 * @brief Restore demo UI (display "on" visual effect)
 *
 * Recreates the demo UI on the active screen.
 * Only works in LVGL mode.
 *
 * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED if not in LVGL mode
 */
esp_err_t display_system_show_ui(void);

/**
 * @brief Deinitialize display system
 *
 * Releases all resources:
 * - Touch controller
 * - PPA accelerator
 * - MIPI DSI bus
 * - LDO power channel
 * - Backlight GPIO
 */
void display_system_deinit(display_handles_t *handles);

#ifdef __cplusplus
}
#endif