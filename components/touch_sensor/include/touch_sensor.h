/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "touch_sensor_config.h"
#include "touch_sensor_types.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file touch_sensor.h
 * @brief TTP223 capacitive touch sensor driver
 *
 * Drives a TTP223 touch sensor via GPIO interrupt with software debounce.
 * TTP223 in sync mode: OUT pin goes HIGH on touch, LOW on release.
 *
 * Provides raw PRESS/RELEASE events via ISR callback.
 * Higher-level gesture recognition is handled by touch_interpreter component.
 */

/**
 * @brief Touch sensor configuration
 */
typedef struct {
    touch_sensor_event_cb_t event_cb; /*!< Callback for raw touch events (ISR context) */
    void *user_ctx;                   /*!< User context passed to callback */
} touch_sensor_config_t;

/**
 * @brief Default configuration
 */
#define TOUCH_SENSOR_DEFAULT_CONFIG() \
    {                                 \
        .event_cb = NULL,             \
        .user_ctx = NULL,             \
    }

/**
 * @brief Initialize touch sensor driver
 *
 * Configures GPIO for TTP223 OUT pin with interrupt on both edges.
 * TTP223 in sync mode: rising edge = touch, falling edge = release.
 *
 * @param config  Configuration (NULL for defaults — no callback)
 * @return ESP_OK on success
 */
esp_err_t touch_sensor_init(const touch_sensor_config_t *config);

/**
 * @brief Deinitialize touch sensor driver
 *
 * Removes GPIO ISR handler and releases resources.
 *
 * @return ESP_OK on success
 */
esp_err_t touch_sensor_deinit(void);

/**
 * @brief Check if touch sensor driver is initialized
 *
 * @return true if initialized
 */
bool touch_sensor_is_initialized(void);

/**
 * @brief Get current touch state
 *
 * Reads GPIO level directly (not debounced).
 * TTP223 sync mode: HIGH = touching, LOW = not touching.
 *
 * @return true if sensor is currently being touched
 */
bool touch_sensor_is_pressed(void);

/* Stub implementations when component is disabled */
#if (TOUCH_SENSOR_ENABLE == 0)

static inline esp_err_t touch_sensor_init(const touch_sensor_config_t *config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t touch_sensor_deinit(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline bool touch_sensor_is_initialized(void)
{
    return false;
}
static inline bool touch_sensor_is_pressed(void)
{
    return false;
}

#endif /* TOUCH_SENSOR_ENABLE == 0 */

#ifdef __cplusplus
}
#endif
