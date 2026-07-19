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

/* MCP tools enable/disable switch (requires esp_xiaozhi component)
 * Set to 0 when esp_xiaozhi is not available, task core functions still work */
#ifndef TASK_MANAGER_ENABLE_MCP
#define TASK_MANAGER_ENABLE_MCP 0
#endif

/* Maximum number of concurrent tasks */
#if (TASK_MANAGER_ENABLE == 1)
#define TASK_MANAGER_MAX_TASKS CONFIG_TASK_MANAGER_MAX_TASKS
#endif

#ifdef __cplusplus
}
#endif
