/**
 * @file light_control_module.h
 * @brief Light control module interface
 *
 * This file provides the interface for the light control module,
 * which controls lights via UART communication with an ESP32-C3 device.
 *
 * The module provides:
 * - Manual light control
 * - UART communication with ESP32-C3 light controller
 *
 * Configuration is managed through Kconfig system:
 * - PROJECT_ENABLE_LIGHT_CONTROL: Enable/disable the module
 * - LIGHT_CONTROL_DEFAULT_BRIGHTNESS: Default LED brightness
 * - LIGHT_CONTROL_DEFAULT_WARM/COLD: Default CCT LED brightness
 * - LIGHT_CONTROL_DEFAULT_COLOR_R/G/B: Default WS2812 color
 *
 * NOTE: Automatic light adjustment based on the ambient light sensor was
 * removed together with the light sensor module. Only manual control remains.
 *
 * @author CottonLin
 * @date 2026-04-28
 * @version 2.0.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "led_types.h"
#include "uart_light_controller.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @defgroup LightControlModule Light Control Module
     * @brief Light control module interface
     * @{
     */

#if (CONFIG_PROJECT_ENABLE_LIGHT_CONTROL == 1)

    /**
     * @brief Initialize the light control module
     *
     * This function initializes the light control module.
     *
     * @note This function initializes the UART light controller internally.
     *
     * @return esp_err_t
     *      - ESP_OK: Success
     *      - ESP_ERR_INVALID_STATE: Module already initialized
     *      - ESP_ERR_NO_MEM: Memory allocation failed
     */
    esp_err_t light_control_module_init(void);

    /**
     * @brief Deinitialize the light control module and stop the task
     *
     * This function stops the light control task and releases all resources.
     *
     * @return esp_err_t
     *      - ESP_OK: Success
     *      - ESP_ERR_INVALID_STATE: Module not initialized
     */
    esp_err_t light_control_module_deinit(void);

    /**
     * @brief Enable or disable automatic light control
     *
     * @param[in] enable true to enable, false to disable
     *
     * @return esp_err_t
     *      - ESP_OK: Success (disable)
     *      - ESP_ERR_INVALID_STATE: Module not initialized
     *      - ESP_ERR_NOT_SUPPORTED: Auto mode not available (light sensor removed)
     */
    esp_err_t light_control_module_set_auto_mode(bool enable);

    /**
     * @brief Check if automatic light control is enabled
     *
     * @return true if enabled, false otherwise
     */
    bool light_control_module_is_auto_mode(void);

    /**
     * @brief Manually set LED brightness
     *
     * This function sets the LED brightness manually, disabling auto mode.
     *
     * @param[in] brightness Brightness level (0-255)
     *
     * @return esp_err_t
     *      - ESP_OK: Success
     *      - ESP_ERR_INVALID_STATE: Module not initialized
     */
    esp_err_t light_control_module_set_brightness(uint8_t brightness);

    /**
     * @brief Manually set LED color
     *
     * This function sets the LED color manually, disabling auto mode.
     *
     * @param[in] color RGB color
     *
     * @return esp_err_t
     *      - ESP_OK: Success
     *      - ESP_ERR_INVALID_STATE: Module not initialized
     */
    esp_err_t light_control_module_set_color(ws2812_rgb_t color);

    /**
     * @brief Manually set all light parameters
     *
     * This function sets all light parameters manually, disabling auto mode.
     *
     * @param[in] params Light control parameters (warm, cold, R, G, B)
     *
     * @return esp_err_t
     *      - ESP_OK: Success
     *      - ESP_ERR_INVALID_STATE: Module not initialized
     *      - ESP_ERR_INVALID_ARG: Invalid parameters
     */
    esp_err_t light_control_module_set_params(const uart_light_params_t* params);

    /**
     * @brief Turn off the LED
     *
     * @return esp_err_t
     *      - ESP_OK: Success
     *      - ESP_ERR_INVALID_STATE: Module not initialized
     */
    esp_err_t light_control_module_turn_off(void);

    /**
     * @brief Get current ambient light intensity
     *
     * @param[out] lux Pointer to store the light intensity value
     *
     * @return esp_err_t
     *      - ESP_OK: Success
     *      - ESP_ERR_INVALID_STATE: Module not initialized
     *      - ESP_ERR_NOT_SUPPORTED: Light sensor not available (module removed)
     */
    esp_err_t light_control_module_get_lux(float* lux);

    /**
     * @brief Check if the module is initialized
     *
     * @return true if initialized, false otherwise
     */
    bool light_control_module_is_initialized(void);

#else

static inline esp_err_t light_control_module_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t light_control_module_deinit(void)
{
    return ESP_OK;
}
static inline esp_err_t light_control_module_set_auto_mode(bool enable)
{
    (void)enable;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline bool light_control_module_is_auto_mode(void)
{
    return false;
}
static inline esp_err_t light_control_module_set_brightness(uint8_t brightness)
{
    (void)brightness;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t light_control_module_set_color(ws2812_rgb_t color)
{
    (void)color;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t light_control_module_set_params(const uart_light_params_t* params)
{
    (void)params;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t light_control_module_turn_off(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t light_control_module_get_lux(float* lux)
{
    (void)lux;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline bool light_control_module_is_initialized(void)
{
    return false;
}

#endif

#ifdef __cplusplus
}
#endif
