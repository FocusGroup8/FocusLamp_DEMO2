/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Raw touch event types from TTP223 sensor
 *
 * TOUCH_EVENT_PRESS:   Finger detected (GPIO went high in sync mode)
 * TOUCH_EVENT_RELEASE: Finger removed (GPIO went low in sync mode)
 */
typedef enum {
    TOUCH_EVENT_PRESS,   /*!< Touch detected (finger on sensor) */
    TOUCH_EVENT_RELEASE, /*!< Touch released (finger off sensor) */
} touch_event_t;

/**
 * @brief Touch sensor event data
 */
typedef struct {
    touch_event_t event;  /*!< Event type */
    int64_t timestamp_ms; /*!< Event timestamp in ms (from esp_timer_get_time()/1000) */
} touch_sensor_event_data_t;

/**
 * @brief Callback for raw touch sensor events
 *
 * Called from GPIO ISR context — must be ISR-safe.
 * For deferred processing, use a queue or task notification.
 *
 * @param event_data  Event data (timestamp, event type)
 * @param user_ctx    User context registered at init
 */
typedef void (*touch_sensor_event_cb_t)(const touch_sensor_event_data_t *event_data, void *user_ctx);

#ifdef __cplusplus
}
#endif
