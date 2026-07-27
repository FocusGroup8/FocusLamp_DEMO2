/*
 * lighting_app.h - Lighting application module for FocusLamp
 */

#pragma once
#ifndef __LIGHTING_APP_H__
#define __LIGHTING_APP_H__

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize lighting application.
 *        Subscribes to EV_APP_MODE_CHANGED and related events.
 * @return esp_err_t
 */
esp_err_t lighting_app_init(void);

/**
 * @brief Start lighting mode.
 * @return esp_err_t
 */
esp_err_t lighting_app_start(void);

/**
 * @brief Stop lighting mode.
 * @return esp_err_t
 */
esp_err_t lighting_app_stop(void);

/**
 * @brief Set brightness level.
 * @param brightness  Brightness value (0-255)
 * @return esp_err_t
 */
esp_err_t lighting_app_set_brightness(uint8_t brightness);

/**
 * @brief Set color temperature in Kelvin.
 * @param temp  Color temperature in K (e.g. 2700-6500)
 * @return esp_err_t
 */
esp_err_t lighting_app_set_colortemp(uint16_t temp);

/**
 * @brief Set RGB color.
 * @param r  Red component (0-255)
 * @param g  Green component (0-255)
 * @param b  Blue component (0-255)
 * @return esp_err_t
 */
esp_err_t lighting_app_set_color(uint8_t r, uint8_t g, uint8_t b);

#ifdef __cplusplus
}
#endif

#endif /* __LIGHTING_APP_H__ */