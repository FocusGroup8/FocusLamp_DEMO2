/*
 * power_driver.h - Power management driver for FocusLamp
 */

#pragma once
#ifndef __POWER_DRIVER_H__
#define __POWER_DRIVER_H__

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize power control driver.
 *        Calls bsp_power_init() to configure power control pins.
 * @return esp_err_t
 */
esp_err_t power_driver_init(void);

/**
 * @brief Set ESP module power on/off.
 * @param on  true = ESP on, false = ESP off
 * @return esp_err_t
 */
esp_err_t power_driver_set_esp_power(bool on);

#ifdef __cplusplus
}
#endif

#endif /* __POWER_DRIVER_H__ */
