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
#define XIAOZHI_MANAGER_ENABLE CONFIG_XIAOZHI_MANAGER_ENABLE

/* OTA URL configuration */
#define XIAOZHI_MANAGER_OTA_URL CONFIG_XIAOZHI_MANAGER_OTA_URL

/* Wake word configuration */
#define XIAOZHI_MANAGER_DEFAULT_WAKE_WORD CONFIG_XIAOZHI_MANAGER_DEFAULT_WAKE_WORD

/* Audio configuration */
#if (XIAOZHI_MANAGER_ENABLE == 1)
#define XIAOZHI_MANAGER_AUDIO_SAMPLE_RATE CONFIG_XIAOZHI_MANAGER_AUDIO_SAMPLE_RATE
#define XIAOZHI_MANAGER_AUDIO_FRAME_DURATION CONFIG_XIAOZHI_MANAGER_AUDIO_FRAME_DURATION
#endif

#ifdef __cplusplus
}
#endif
