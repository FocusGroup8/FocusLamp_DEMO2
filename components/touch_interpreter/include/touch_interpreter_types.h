/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Recognized touch gesture types
 */
typedef enum {
    TOUCH_GESTURE_TAP,        /*!< Single tap (short touch, 100-300ms) */
    TOUCH_GESTURE_DOUBLE_TAP, /*!< Double tap (two consecutive taps within 200-500ms) */
    TOUCH_GESTURE_LONG_PRESS, /*!< Long press (touch held > 2 seconds) */
} touch_gesture_t;

/**
 * @brief Gesture event data
 */
typedef struct {
    touch_gesture_t gesture; /*!< Recognized gesture type */
    int64_t timestamp_ms;    /*!< Gesture completion timestamp in ms */
    int64_t duration_ms;     /*!< Touch duration in ms (for TAP and LONG_PRESS) */
    int64_t interval_ms;     /*!< Interval between two taps in ms (for DOUBLE_TAP only) */
} touch_gesture_event_t;

/**
 * @brief Callback for recognized gesture events
 *
 * Called from a FreeRTOS task context (not ISR).
 * Safe to call most ESP-IDF APIs from this callback.
 *
 * @param event  Gesture event data
 * @param ctx    User context
 */
typedef void (*touch_gesture_cb_t)(const touch_gesture_event_t *event, void *ctx);

#ifdef __cplusplus
}
#endif
