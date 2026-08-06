/*
 * lamp_head_controller.h - Remote lamp head (camera-fps board) controller
 *
 * Sends HTTP REST API calls to the lamp head board to control:
 * - Head LED (on/off/brightness)
 * - Head expression (big screen expressive eyes)
 *
 * Target IP is configured independently via LAMP_HEAD_TARGET_IP (menuconfig),
 * separate from status_reporter's target (voice board).
 */

#pragma once
#ifndef __LAMP_HEAD_CONTROLLER_H__
#define __LAMP_HEAD_CONTROLLER_H__

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Turn on head LED with specified brightness.
 * @param brightness  Brightness percentage (0-100)
 * @return ESP_OK on success
 */
esp_err_t lamp_head_led_on(uint8_t brightness);

/**
 * @brief Turn on head LED with brightness automatically adjusted
 *        based on ambient light sensor reading.
 * @return ESP_OK on success
 */
esp_err_t lamp_head_led_on_with_ambient(void);

/**
 * @brief Turn off head LED.
 * @return ESP_OK on success
 */
esp_err_t lamp_head_led_off(void);

/**
 * @brief Set head expression on big screen.
 * @param expression  Expression string: "neutral", "happy", "sad", "angry",
 *                    "surprised", "sleepy", "bored", "wink_left", "wink_right"
 * @return ESP_OK on success
 */
esp_err_t lamp_head_set_expression(const char *expression);

#ifdef __cplusplus
}
#endif

#endif /* __LAMP_HEAD_CONTROLLER_H__ */
