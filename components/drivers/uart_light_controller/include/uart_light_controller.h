/**
 * @file uart_light_controller.h
 * @brief UART light controller interface
 *
 * This file provides the interface for controlling lights via UART.
 * The controller sends commands to an ESP32-C3 device that directly
 * drives WS2812 LEDs and CCT LED panels.
 *
 * Command format: "W:{warm},C:{cold},R:{red},G:{green},B:{blue}\n"
 * Example: "W:100,C:200,R:255,G:128,B:64\n"
 *
 * @author CottonLin
 * @date 2026-04-28
 * @version 1.0.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @defgroup UARTLightController UART Light Controller
     * @brief UART-based light controller interface
     * @{
     */

#if (CONFIG_PROJECT_ENABLE_UART_LIGHT_CONTROLLER == 1)

    /**
     * @brief Light control parameters
     */
    typedef struct
    {
        uint16_t warm;  /**< Warm LED brightness (0-8191, 13-bit PWM) */
        uint16_t cold;  /**< Cold LED brightness (0-8191, 13-bit PWM) */
        uint8_t  red;   /**< Red component (0-255) */
        uint8_t  green; /**< Green component (0-255) */
        uint8_t  blue;  /**< Blue component (0-255) */
    } uart_light_params_t;

    /**
     * @brief Initialize the UART light controller
     *
     * This function initializes the UART peripheral for communication
     * with the ESP32-C3 light controller.
     *
     * @return ESP_OK on success
     * @return ESP_ERR_INVALID_STATE if already initialized
     * @return ESP_ERR_NO_MEM if memory allocation fails
     */
    esp_err_t uart_light_controller_init(void);

    /**
     * @brief Deinitialize the UART light controller
     *
     * @return ESP_OK on success
     */
    esp_err_t uart_light_controller_deinit(void);

    /**
     * @brief Set light parameters
     *
     * This function sends light control parameters to the ESP32-C3 device.
     *
     * @param params Light control parameters
     * @return ESP_OK on success
     * @return ESP_ERR_INVALID_STATE if not initialized
     * @return ESP_ERR_INVALID_ARG if params is NULL
     */
    esp_err_t uart_light_controller_set_params(const uart_light_params_t* params);

    /**
     * @brief Turn off all lights
     *
     * This function sends a command to turn off all lights.
     *
     * @return ESP_OK on success
     * @return ESP_ERR_INVALID_STATE if not initialized
     */
    esp_err_t uart_light_controller_turn_off(void);

    /**
     * @brief Check if the controller is initialized
     *
     * @return true if initialized
     * @return false if not initialized
     */
    bool uart_light_controller_is_initialized(void);

#else

typedef struct
{
    uint16_t warm;
    uint16_t cold;
    uint8_t  red;
    uint8_t  green;
    uint8_t  blue;
} uart_light_params_t;

static inline esp_err_t uart_light_controller_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t uart_light_controller_deinit(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t uart_light_controller_set_params(const uart_light_params_t* params)
{
    (void)params;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t uart_light_controller_turn_off(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline bool uart_light_controller_is_initialized(void)
{
    return false;
}

#endif

#ifdef __cplusplus
}
#endif