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
#define MIPI_DSI_BRIDGE_ENABLE CONFIG_MIPI_DSI_BRIDGE_ENABLE

/* Target mipi_dsi device IP address */
#define MIPI_DSI_BRIDGE_TARGET_IP CONFIG_MIPI_DSI_BRIDGE_TARGET_IP

/* HTTP request timeout in milliseconds */
#define MIPI_DSI_BRIDGE_HTTP_TIMEOUT_MS 3000

#ifdef __cplusplus
}
#endif
