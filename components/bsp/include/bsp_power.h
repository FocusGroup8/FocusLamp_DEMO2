/*
 * bsp_power.h - Power management for FocusLamp BSP
 */

#pragma once
#ifndef __BSP_POWER_H__
#define __BSP_POWER_H__

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize power control.
 *        Sets ESP_EN to output high level.
 */
esp_err_t bsp_power_init(void);

/**
 * @brief Enable or disable ESP module power (ESP_EN)
 * @param enable  true = ESP on, false = ESP off
 * @return esp_err_t
 */
esp_err_t bsp_power_set_esp_enable(bool enable);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_POWER_H__ */
