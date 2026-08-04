/*
 * led_driver.h - WS2812 LED strip driver for FocusLamp
 * Enhanced with RMT-based WS2812 control, snapshot, and effect helpers.
 */

#pragma once
#ifndef __LED_DRIVER_H__
#define __LED_DRIVER_H__

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#include "led_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize WS2812 LED strip driver using RMT peripheral.
 *        LED count is read from system_config.h (LED_NUM_LEDS).
 * @return esp_err_t
 */
esp_err_t led_driver_init(void);

/**
 * @brief Deinitialize LED driver and release RMT resources.
 * @return esp_err_t
 */
esp_err_t led_driver_deinit(void);

/**
 * @brief Set color of a single LED.
 * @param index  LED index (0-based)
 * @param r      Red component (0-255)
 * @param g      Green component (0-255)
 * @param b      Blue component (0-255)
 * @return esp_err_t
 */
esp_err_t led_driver_set_color(uint8_t index, uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Set all LEDs to the same color.
 * @param r  Red component (0-255)
 * @param g  Green component (0-255)
 * @param b  Blue component (0-255)
 * @return esp_err_t
 */
esp_err_t led_driver_set_all(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Refresh / latch the LED strip to display buffered colors.
 * @return esp_err_t
 */
esp_err_t led_driver_show(void);

/**
 * @brief Set global brightness scaling factor.
 * @param brightness  Brightness value (0-255)
 */
void led_driver_set_brightness(uint8_t brightness);

/**
 * @brief Get current brightness level.
 * @return uint8_t Current brightness (0-255)
 */
uint8_t led_driver_get_brightness(void);

/**
 * @brief Turn off all LEDs immediately.
 * @return esp_err_t
 */
esp_err_t led_driver_clear(void);

/**
 * @brief Check if driver is initialized.
 * @return true if initialized
 */
bool led_driver_is_initialized(void);

/**
 * @brief Get current driver state snapshot.
 * @param[out] snapshot  Pointer to fill with current state
 * @return esp_err_t
 */
esp_err_t led_driver_get_snapshot(led_module_snapshot_t* snapshot);

/**
 * @brief Apply brightness scaling to raw RGB values.
 * @param r  Red input (0-255)
 * @param g  Green input (0-255)
 * @param b  Blue input (0-255)
 * @param brightness  Brightness factor (0-255)
 * @param[out] out_r  Scaled red output
 * @param[out] out_g  Scaled green output
 * @param[out] out_b  Scaled blue output
 */
void led_driver_scale_brightness(uint8_t r, uint8_t g, uint8_t b,
                                 uint8_t brightness,
                                 uint8_t* out_r, uint8_t* out_g, uint8_t* out_b);

#ifdef __cplusplus
}
#endif

#endif /* __LED_DRIVER_H__ */
