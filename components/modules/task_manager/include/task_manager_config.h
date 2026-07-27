/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once
#ifndef __TASK_MANAGER_CONFIG_H__
#define __TASK_MANAGER_CONFIG_H__

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TASK_MANAGER_ENABLE CONFIG_TASK_MANAGER_ENABLE

#if (TASK_MANAGER_ENABLE == 1)
#define TASK_MANAGER_MAX_TASKS CONFIG_TASK_MANAGER_MAX_TASKS
#endif

#ifdef __cplusplus
}
#endif

#endif /* __TASK_MANAGER_CONFIG_H__ */
