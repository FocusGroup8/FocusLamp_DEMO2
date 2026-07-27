/*
 * radar_module_config.h - Radar module configuration
 */

#pragma once
#ifndef __RADAR_MODULE_CONFIG_H__
#define __RADAR_MODULE_CONFIG_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== UART Buffer & Timeout ===================== */
#define RADAR_MODULE_UART_BUFFER_SIZE            (1024)
#define RADAR_MODULE_DATA_TIMEOUT_MS             (5000)

/* ===================== Queue Sizes ===================== */
#define RADAR_MODULE_FRAME_QUEUE_SIZE            (10)
#define RADAR_MODULE_EVENT_QUEUE_SIZE            (16)
#define RADAR_MODULE_MAX_CONSECUTIVE_ERRORS      (10)

/* ===================== Feature Flags (set from Kconfig / system_config) ===================== */
#ifndef RADAR_MODULE_LOG_ENABLE
#define RADAR_MODULE_LOG_ENABLE                  (0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* __RADAR_MODULE_CONFIG_H__ */
