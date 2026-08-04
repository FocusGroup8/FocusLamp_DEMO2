/*
 * power_service.h - Power management service for FocusLamp
 */

#pragma once
#ifndef __POWER_SERVICE_H__
#define __POWER_SERVICE_H__

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== Power Mode Enumeration ===================== */
typedef enum {
    POWER_MODE_NORMAL   = 0,
    POWER_MODE_SAVING,      /* 省电模式 */
    POWER_MODE_SLEEP,       /* 休眠模式 */
} power_mode_t;

/**
 * @brief Initialize power service.
 *        Subscribes to EV_POWER_* events and initializes power_driver.
 * @return esp_err_t
 */
esp_err_t power_service_init(void);

/**
 * @brief Set power management mode.
 * @param mode  Power mode to set
 * @return esp_err_t
 */
esp_err_t power_service_set_power_mode(power_mode_t mode);

/**
 * @brief Get current power management mode.
 * @return power_mode_t Current mode
 */
power_mode_t power_service_get_power_mode(void);

/**
 * @brief Shutdown system gracefully.
 */
void power_service_shutdown(void);

/**
 * @brief Get current battery level.
 * @return uint8_t  Battery level 0-100 (percent), 0xFF if unknown
 */
uint8_t power_service_get_battery_level(void);

#ifdef __cplusplus
}
#endif

#endif /* __POWER_SERVICE_H__ */
