/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file expressive_eyes_display.h
 * @brief Expressive Eyes display module (C bridge for espp::ExpressiveEyes)
 *
 * Provides animated expressive eyes on the MIPI DSI display using the
 * espp/expressive_eyes C++ component. This module acts as a C/C++ bridge,
 * wrapping the C++ implementation behind a C interface for integration
 * with the existing display_system module.
 *
 * Uses Monochrome Blue drawer style (electric blue eyes on black background).
 */

/**
 * @brief Expressive eyes expression types (mirrors espp::ExpressiveEyes::Expression)
 */
typedef enum {
    EXPRESSIVE_EYES_NEUTRAL,
    EXPRESSIVE_EYES_HAPPY,
    EXPRESSIVE_EYES_SAD,
    EXPRESSIVE_EYES_ANGRY,
    EXPRESSIVE_EYES_SURPRISED,
    EXPRESSIVE_EYES_SLEEPY,
    EXPRESSIVE_EYES_BORED,
    EXPRESSIVE_EYES_WINK_LEFT,
    EXPRESSIVE_EYES_WINK_RIGHT,
} expressive_eyes_expression_t;

/**
 * @brief Expressive eyes configuration
 */
typedef struct {
    lv_display_t *disp;        /*!< LVGL display handle (from lvgl_display_t) */
    int screen_width;          /*!< Screen width in pixels */
    int screen_height;         /*!< Screen height in pixels */
    float blink_duration;      /*!< Blink duration in seconds (default: 0.12) */
    float blink_interval;      /*!< Average time between blinks (default: 4.0) */
    bool enable_auto_blink;    /*!< Enable automatic random blinking */
    bool enable_pupil_physics; /*!< Enable smooth pupil movement */
} expressive_eyes_cfg_t;

/**
 * @brief Initialize expressive eyes display
 *
 * Creates LVGL canvas, allocates PSRAM buffer, initializes the
 * espp::ExpressiveEyes C++ object with Monochrome Blue drawer,
 * and starts the animation FreeRTOS task.
 *
 * @param cfg  Configuration (must provide disp, screen_width, screen_height)
 * @return ESP_OK on success
 */
esp_err_t expressive_eyes_display_init(const expressive_eyes_cfg_t *cfg);

/**
 * @brief Deinitialize expressive eyes display
 *
 * Stops the animation task, releases the C++ object and PSRAM canvas buffer.
 *
 * @return ESP_OK on success
 */
esp_err_t expressive_eyes_display_deinit(void);

/**
 * @brief Set expression
 *
 * @param expr  Expression to display
 */
void expressive_eyes_set_expression(expressive_eyes_expression_t expr);

/**
 * @brief Set look direction
 *
 * @param x  Horizontal direction (-1.0 to 1.0, 0=center)
 * @param y  Vertical direction (-1.0 to 1.0, 0=center)
 */
void expressive_eyes_look_at(float x, float y);

/**
 * @brief Trigger a blink
 */
void expressive_eyes_blink(void);

/**
 * @brief Get current expression
 *
 * @return Current expression type
 */
expressive_eyes_expression_t expressive_eyes_get_expression(void);

#ifdef __cplusplus
}
#endif
