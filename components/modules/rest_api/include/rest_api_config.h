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
#ifdef CONFIG_REST_API_ENABLE
#define REST_API_ENABLE CONFIG_REST_API_ENABLE
#else
#define REST_API_ENABLE 0
#endif

#ifdef __cplusplus
}
#endif
