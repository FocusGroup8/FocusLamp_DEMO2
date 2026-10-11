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
#define TOUCH_SENSOR_ENABLE CONFIG_TOUCH_SENSOR_ENABLE

/* GPIO pin for TTP223 touch sensor input */
#if (TOUCH_SENSOR_ENABLE == 1)
#define TOUCH_SENSOR_GPIO CONFIG_TOUCH_SENSOR_GPIO
#endif

/* Software debounce time in ms */
#if (TOUCH_SENSOR_ENABLE == 1)
#define TOUCH_SENSOR_DEBOUNCE_MS CONFIG_TOUCH_SENSOR_DEBOUNCE_MS
#endif

/* Minimum touch event interval in ms (anti-spam filter) */
#if (TOUCH_SENSOR_ENABLE == 1)
#define TOUCH_SENSOR_MIN_INTERVAL_MS CONFIG_TOUCH_SENSOR_MIN_INTERVAL_MS
#endif

#ifdef __cplusplus
}
#endif
