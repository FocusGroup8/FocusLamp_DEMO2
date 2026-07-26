/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "touch_interpreter_config.h"
#include "touch_interpreter_types.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file touch_interpreter.h
 * @brief Touch gesture recognition engine
 *
 * Consumes raw PRESS/RELEASE events from touch_sensor and recognizes:
 * - TAP: short touch (100-300ms)
 * - DOUBLE_TAP: two taps within 200-500ms interval
 * - LONG_PRESS: touch held > 2 seconds
 *
 * Uses a FreeRTOS task + queue to process events off-ISR.
 * State machine handles disambiguation between TAP and DOUBLE_TAP
 * by waiting for the double-tap timeout after the first TAP.
 */

/**
 * @brief Touch interpreter configuration
 */
typedef struct {
    touch_gesture_cb_t gesture_cb; /*!< Callback for recognized gestures */
    void *user_ctx;                /*!< User context passed to callback */
    int task_stack_size;           /*!< Processing task stack size (default 3072) */
    int task_priority;             /*!< Processing task priority (default 5) */
} touch_interpreter_config_t;

/**
 * @brief Default configuration
 */
#define TOUCH_INTERPRETER_DEFAULT_CONFIG() \
    {                                      \
        .gesture_cb      = NULL,           \
        .user_ctx        = NULL,           \
        .task_stack_size = 3072,           \
        .task_priority   = 5,              \
    }

/**
 * @brief Initialize touch interpreter
 *
 * Creates event queue and processing task.
 * Automatically registers as callback on touch_sensor.
 * Requires touch_sensor to be initialized first.
 *
 * @param config  Configuration (NULL for defaults)
 * @return ESP_OK on success
 */
esp_err_t touch_interpreter_init(const touch_interpreter_config_t *config);

/**
 * @brief Deinitialize touch interpreter
 *
 * Stops processing task and releases resources.
 * Does not deinitialize touch_sensor.
 *
 * @return ESP_OK on success
 */
esp_err_t touch_interpreter_deinit(void);

/**
 * @brief Check if touch interpreter is initialized
 *
 * @return true if initialized
 */
bool touch_interpreter_is_initialized(void);

/* Stub implementations when component is disabled */
#if (TOUCH_INTERPRETER_ENABLE == 0)

static inline esp_err_t touch_interpreter_init(const touch_interpreter_config_t *config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t touch_interpreter_deinit(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline bool touch_interpreter_is_initialized(void)
{
    return false;
}

#endif /* TOUCH_INTERPRETER_ENABLE == 0 */

#ifdef __cplusplus
}
#endif
