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
#define TASK_MANAGER_ENABLE CONFIG_TASK_MANAGER_ENABLE

/* Maximum number of concurrent tasks */
#if (TASK_MANAGER_ENABLE == 1)
#define TASK_MANAGER_MAX_TASKS CONFIG_TASK_MANAGER_MAX_TASKS
#endif

#ifdef __cplusplus
}
#endif
