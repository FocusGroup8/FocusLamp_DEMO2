/*
 * bsp_gpio.h - GPIO initialization and control for FocusLamp BSP
 */

#pragma once
#ifndef __BSP_GPIO_H__
#define __BSP_GPIO_H__

#include <stdint.h>
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize all output GPIOs:
 *        ESP_EN, LED_DIN, LCD_BL, LCD_DC, LCD_CS, SERVO_OE1, SERVO_OE2
 *        Sets pin direction and default level according to pin_config.h
 */
esp_err_t bsp_gpio_init(void);

/**
 * @brief Set level of a GPIO pin
 * @param gpio  GPIO number
 * @param level 0 or 1
 * @return esp_err_t
 */
esp_err_t bsp_gpio_set_level(gpio_num_t gpio, uint32_t level);

/**
 * @brief Get level of a GPIO pin
 * @param gpio  GPIO number
 * @return int   0 or 1, -1 on error
 */
int bsp_gpio_get_level(gpio_num_t gpio);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_GPIO_H__ */