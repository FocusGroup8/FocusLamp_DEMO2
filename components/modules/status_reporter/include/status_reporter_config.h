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
#define STATUS_REPORTER_ENABLE CONFIG_STATUS_REPORTER_ENABLE

/* Target wifi_test device IP address (set via menuconfig) */
#define STATUS_REPORTER_TARGET_IP CONFIG_STATUS_REPORTER_TARGET_IP

/* HTTP request timeout in milliseconds */
#define STATUS_REPORTER_HTTP_TIMEOUT_MS 3000

/* Periodic reporting interval in milliseconds */
#define STATUS_REPORTER_PERIODIC_INTERVAL_MS 10000

/* Status report endpoint URI on wifi_test */
#define STATUS_REPORTER_ENDPOINT_URI "/api/status/report"

/* Maximum JSON body size */
#define STATUS_REPORTER_JSON_BUF_SIZE 1024

/* Minimum interval between event-triggered reports (milliseconds, for debouncing) */
#define STATUS_REPORTER_EVENT_DEBOUNCE_MS 500

#ifdef __cplusplus
}
#endif
