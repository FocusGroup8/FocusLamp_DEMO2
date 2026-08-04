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
#define FOCUSLAMP_BRIDGE_ENABLE CONFIG_FOCUSLAMP_BRIDGE_ENABLE

/* Target FocusLamp device IP address */
#define FOCUSLAMP_BRIDGE_TARGET_IP CONFIG_FOCUSLAMP_BRIDGE_TARGET_IP

/* HTTP request timeout in milliseconds */
#define FOCUSLAMP_BRIDGE_HTTP_TIMEOUT_MS 10000

#ifdef __cplusplus
}
#endif
