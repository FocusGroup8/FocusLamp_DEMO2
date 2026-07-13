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
#define DEVICE_CONTROLLER_ENABLE CONFIG_DEVICE_CONTROLLER_ENABLE

/* Light configuration */
#if (DEVICE_CONTROLLER_ENABLE == 1)
#define DEVICE_CONTROLLER_LIGHT_MAX_BRIGHTNESS  100
#define DEVICE_CONTROLLER_LIGHT_MIN_BRIGHTNESS  0
#define DEVICE_CONTROLLER_LIGHT_BRIGHTNESS_STEP 20
#endif

/* Speaker configuration */
#define DEVICE_CONTROLLER_DEFAULT_VOLUME 50

#ifdef __cplusplus
}
#endif
