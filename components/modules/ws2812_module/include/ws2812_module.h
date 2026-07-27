/**
 * @file ws2812_module.h
 * @brief WS2812 LED driver module interface
 *
 * This file provides the interface for the WS2812 LED driver module,
 * which controls WS2812 RGB LEDs using the RMT peripheral.
 *
 * The module provides:
 * - LED initialization and deinitialization
 * - RGB color setting
 * - Brightness control
 * - Breath animation effect
 *
 * Configuration is managed through Kconfig system:
 * - RADAR_TEST_WS2812_GPIO: Data GPIO pin
 * - RADAR_TEST_WS2812_LED_COUNT: Number of LEDs
 *
 * @author CottonLin
 * @date 2026-04-23
 * @version 1.0.0
 *
 * @see led_types.h for LED type definitions
 * @see docs/guides/led_guide.md for usage guide
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "led_types.h"
#include "ws2812_module_config.h"

#ifdef __cplusplus
extern "C"
{
#endif

#if (WS2812_MODULE_ENABLE == 1)

    /**
     * @brief Initialize the WS2812 module using project default GPIO
     *
     * This function initializes the RMT peripheral and configures the
     * WS2812 LED driver with parameters from Kconfig.
     *
     * @note This function must be called before any other module functions
     * @note Configuration is read from Kconfig (WS2812_GPIO, WS2812_LED_COUNT)
     *
     * @return esp_err_t
     *      - ESP_OK: Success
     *      - ESP_ERR_INVALID_STATE: Module already initialized
     *      - ESP_ERR_NO_MEM: Memory allocation failed
     *      - Other error codes from RMT driver
     */
    esp_err_t ws2812_module_init(void);

    /**
     * @brief Deinitialize the WS2812 module and release RMT resources
     *
     * This function stops the RMT peripheral and releases all resources.
     *
     * @return esp_err_t
     *      - ESP_OK: Success
     *      - ESP_ERR_INVALID_STATE: Module not initialized
     */
    esp_err_t ws2812_module_deinit(void);

    /**
     * @brief Set the current pixel color and send it immediately
     *
     * This function sets the RGB color of the LED and sends the data
     * to the WS2812 LED strip immediately.
     *
     * @param[in] red Red channel value (0-255)
     * @param[in] green Green channel value (0-255)
     * @param[in] blue Blue channel value (0-255)
     *
     * @return esp_err_t
     *      - ESP_OK: Success
     *      - ESP_ERR_INVALID_STATE: Module not initialized
     */
    esp_err_t ws2812_module_set_rgb(uint8_t red, uint8_t green, uint8_t blue);

    /**
     * @brief Set brightness scaling for subsequent output
     *
     * This function sets the brightness level for all subsequent
     * color operations. The brightness is applied as a scaling factor.
     *
     * @param[in] brightness Brightness scale in range 0-255
     *                  - 0: LED off
     *                  - 255: Full brightness
     *
     * @return esp_err_t
     *      - ESP_OK: Success
     *      - ESP_ERR_INVALID_STATE: Module not initialized
     */
    esp_err_t ws2812_module_set_brightness(uint8_t brightness);

    /**
     * @brief Turn off the LED immediately
     *
     * This function sets all LEDs to black (off) and sends the data.
     *
     * @return esp_err_t
     *      - ESP_OK: Success
     *      - ESP_ERR_INVALID_STATE: Module not initialized
     */
    esp_err_t ws2812_module_clear(void);

    /**
     * @brief Render one step of the default breathing effect
     *
     * This function calculates and sets the LED brightness based on
     * elapsed time to create a breathing animation effect.
     *
     * @param[in] elapsed_ms Elapsed time in milliseconds
     *
     * @return esp_err_t
     *      - ESP_OK: Success
     *      - ESP_ERR_INVALID_STATE: Module not initialized
     */
    esp_err_t ws2812_module_show_breath_step(uint32_t elapsed_ms);

    /**
     * @brief Check if the module has been initialized
     *
     * @return true Module is initialized and ready
     * @return false Module is not initialized
     */
    bool ws2812_module_is_initialized(void);

#else

static inline esp_err_t ws2812_module_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t ws2812_module_deinit(void)
{
    return ESP_OK;
}
static inline esp_err_t ws2812_module_set_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    (void)red;
    (void)green;
    (void)blue;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t ws2812_module_set_brightness(uint8_t brightness)
{
    (void)brightness;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t ws2812_module_clear(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t ws2812_module_show_breath_step(uint32_t elapsed_ms)
{
    (void)elapsed_ms;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline bool ws2812_module_is_initialized(void)
{
    return false;
}

#endif

    /** @} */

#ifdef __cplusplus
}
#endif
