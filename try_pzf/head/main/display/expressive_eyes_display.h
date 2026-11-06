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
 * @brief Shared robot-face display: FOLA default and eight pixel roles.
 * Owns one RGB565 canvas and the focus/task/default display priority.
 */

/**
 * @brief Stable nine-value expression protocol used by existing controllers.
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
    bool touch_available;     /*!< Existing LVGL GT911 input registration succeeded */
} expressive_eyes_cfg_t;

/**
 * @brief Initialize expressive eyes display
 *
 * Creates the shared LVGL canvas and starts the animation FreeRTOS task.
 * Restores the saved role/style before enabling the panel backlight.
 *
 * @param cfg  Configuration (must provide disp, screen_width, screen_height)
 * @return ESP_OK on success
 */
esp_err_t expressive_eyes_display_init(const expressive_eyes_cfg_t *cfg);

/**
 * @brief Deinitialize expressive eyes display
 *
 * Stops the animation task and releases renderer caches and the PSRAM canvas.
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

/** Select fola (muted teal default), yoyo, diggy, dilly,
 * dash, scout, phew, chacha or tock. Persist with expressive_eyes_save_selection();
 * without a saved selection the face starts as FOLA. legacy remains an input alias. */
esp_err_t expressive_eyes_set_role(const char *role);
const char *expressive_eyes_get_role(void);
/** Optional pixel-role renderer; FOLA uses its own renderer with style original. */
esp_err_t expressive_eyes_set_style(const char *style);
const char *expressive_eyes_get_style(void);
esp_err_t expressive_eyes_save_selection(void);
/** FOLA's nine expressions plus thinking/confused/listening/speaking.
 * Pixel roles retain their nine shapes through explicit fallback mappings. */
esp_err_t expressive_eyes_set_fola_label(const char *label);
const char *expressive_eyes_get_fola_label(void);

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
