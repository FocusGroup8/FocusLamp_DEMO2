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

/* Light configuration — delegated to mipi_dsi_bridge */

/* Speaker configuration */
#define DEVICE_CONTROLLER_DEFAULT_VOLUME 50

#ifdef __cplusplus
}
#endif
