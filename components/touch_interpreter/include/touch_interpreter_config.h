/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Component enable/disable switch */
#define TOUCH_INTERPRETER_ENABLE CONFIG_TOUCH_INTERPRETER_ENABLE

/* Requires touch_sensor component */
#if (TOUCH_INTERPRETER_ENABLE == 1)
#if (CONFIG_TOUCH_SENSOR_ENABLE != 1)
#error "Touch Interpreter requires Touch Sensor component to be enabled"
#endif
#endif

/* Tap duration range in ms */
#if (TOUCH_INTERPRETER_ENABLE == 1)
#define TOUCH_INTERPRETER_TAP_MIN_MS CONFIG_TOUCH_INTERPRETER_TAP_MIN_MS
#define TOUCH_INTERPRETER_TAP_MAX_MS CONFIG_TOUCH_INTERPRETER_TAP_MAX_MS
#endif

/* Double-tap interval range in ms (time between two consecutive taps) */
#if (TOUCH_INTERPRETER_ENABLE == 1)
#define TOUCH_INTERPRETER_DOUBLE_TAP_MIN_MS CONFIG_TOUCH_INTERPRETER_DOUBLE_TAP_MIN_MS
#define TOUCH_INTERPRETER_DOUBLE_TAP_MAX_MS CONFIG_TOUCH_INTERPRETER_DOUBLE_TAP_MAX_MS
#endif

/* Long press threshold in ms */
#if (TOUCH_INTERPRETER_ENABLE == 1)
#define TOUCH_INTERPRETER_LONG_PRESS_MS CONFIG_TOUCH_INTERPRETER_LONG_PRESS_MS
#endif

/* Valid touch duration range in ms (anti-noise filter) */
#if (TOUCH_INTERPRETER_ENABLE == 1)
#define TOUCH_INTERPRETER_VALID_TOUCH_MIN_MS CONFIG_TOUCH_INTERPRETER_VALID_TOUCH_MIN_MS
#define TOUCH_INTERPRETER_VALID_TOUCH_MAX_MS CONFIG_TOUCH_INTERPRETER_VALID_TOUCH_MAX_MS
#endif

#ifdef __cplusplus
}
#endif
