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
#define STATUS_RECEIVER_ENABLE CONFIG_STATUS_RECEIVER_ENABLE

/* Status report endpoint URI */
#define STATUS_RECEIVER_ENDPOINT_URI "/api/status/report"

/* Maximum JSON body size for status report (bytes) */
#define STATUS_RECEIVER_MAX_BODY_SIZE 1024

/* Status data staleness threshold (milliseconds) */
#define STATUS_RECEIVER_STALE_TIMEOUT_MS 30000

#ifdef __cplusplus
}
#endif
