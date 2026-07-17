/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file led_controller.h
 * @brief Dual-channel PWM LED controller for cool/warm white LED control
 *
 * Controls two LED channels via LEDC PWM:
 * - LED_A (cool white) on GPIO 29
 * - LED_B (warm white) on GPIO 28
 *
 * Supports independent brightness control and combined color temperature adjustment.
 */

#define LED_GPIO_A GPIO_NUM_29 /*!< LED_A warm white PWM pin (PWM_A = 暖光) */
#define LED_GPIO_B GPIO_NUM_28 /*!< LED_B cool white PWM pin (PWM_B = 冷光) */

#define LED_PWM_FREQ_HZ 5000               /*!< PWM frequency 5kHz (above visible flicker) */
#define LED_PWM_DUTY_RES LEDC_TIMER_10_BIT /*!< 10-bit duty resolution (0-1023) */
#define LED_PWM_MAX_DUTY 1023              /*!< Full-scale duty value for 10-bit resolution */
#define LED_MAX_DUTY_PERCENT 50            /*!< Safety limit: max PWM duty capped at 50% to prevent over-brightness */

#define LED_CCT_MIN 0          /*!< Minimum color temperature (warmest) */
#define LED_CCT_MAX 100        /*!< Maximum color temperature (coolest) */
#define LED_BRIGHTNESS_MIN 0   /*!< Minimum brightness (off) */
#define LED_BRIGHTNESS_MAX 100 /*!< Maximum brightness (full on) */

/**
 * @brief LED channel identifiers
 */
typedef enum {
    LED_CHANNEL_A = 0, /*!< Warm white LED (GPIO 29, PWM_A = 暖光) */
    LED_CHANNEL_B = 1, /*!< Cool white LED (GPIO 28, PWM_B = 冷光) */
    LED_CHANNEL_MAX
} led_channel_t;

/**
 * @brief LED controller configuration
 */
typedef struct {
    uint8_t brightness; /*!< Initial brightness 0-100% */
    uint8_t color_temp; /*!< Initial color temperature 0-100% (0=warm, 100=cool) */
} led_config_t;

/**
 * @brief Initialize LED controller
 *
 * Configures LEDC timer and two PWM channels for LED_A and LED_B.
 * Initial state: both LEDs off (duty = 0).
 *
 * @return ESP_OK on success
 */
esp_err_t led_controller_init(void);

/**
 * @brief Deinitialize LED controller
 *
 * Stops PWM output and releases LEDC resources.
 *
 * @return ESP_OK on success
 */
esp_err_t led_controller_deinit(void);

/**
 * @brief Set brightness of a specific LED channel
 *
 * @param channel  LED channel (LED_CHANNEL_A or LED_CHANNEL_B)
 * @param percent  Brightness percentage (0-100)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on invalid parameters
 */
esp_err_t led_set_brightness(led_channel_t channel, uint8_t percent);

/**
 * @brief Set brightness of both LED channels simultaneously
 *
 * @param brightness_a  LED_A brightness percentage (0-100)
 * @param brightness_b  LED_B brightness percentage (0-100)
 * @return ESP_OK on success
 */
esp_err_t led_set_both_brightness(uint8_t brightness_a, uint8_t brightness_b);

/**
 * @brief Set color temperature by adjusting cool/warm ratio
 *
 * Higher color_temp value shifts towards cool white (LED_A brighter),
 * lower value shifts towards warm white (LED_B brighter).
 * Current brightness level is preserved.
 *
 * @param color_temp  Color temperature 0-100 (0=warmest, 100=coolest)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on invalid parameters
 */
esp_err_t led_set_color_temp(uint8_t color_temp);

/**
 * @brief Set overall brightness while maintaining current color temperature
 *
 * @param brightness  Brightness percentage (0-100)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on invalid parameters
 */
esp_err_t led_set_brightness_with_cct(uint8_t brightness);

/**
 * @brief Turn off both LEDs
 *
 * @return ESP_OK on success
 */
esp_err_t led_off(void);

/**
 * @brief Check if LED controller is initialized
 *
 * @return true if initialized
 */
bool led_is_initialized(void);

#ifdef __cplusplus
}
#endif
