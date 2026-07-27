/**
 * @file uart_light_controller_config.h
 * @brief UART light controller configuration header
 *
 * This file maps Kconfig options to C macros for the UART light controller.
 *
 * @author CottonLin
 * @date 2026-04-28
 * @version 1.0.0
 */

#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @defgroup UARTLightControllerConfig UART Light Controller Configuration
 * @brief Configuration macros for UART light controller
 * @{
 */

/** @brief Enable UART light controller */
#define UART_LIGHT_CONTROLLER_ENABLE CONFIG_PROJECT_ENABLE_UART_LIGHT_CONTROLLER

#if (UART_LIGHT_CONTROLLER_ENABLE == 1)

/** @brief UART baud rate */
#define UART_LIGHT_CONTROLLER_BAUD_RATE CONFIG_UART_LIGHT_CONTROLLER_BAUD_RATE

/** @brief UART buffer size */
#define UART_LIGHT_CONTROLLER_BUF_SIZE CONFIG_UART_LIGHT_CONTROLLER_BUF_SIZE

/** @brief UART port number (based on preset) */
#if defined(CONFIG_UART_LIGHT_CONTROLLER_PRESET_DEVELOPMENT)
#define UART_LIGHT_CONTROLLER_PORT 4
#define UART_LIGHT_CONTROLLER_TX_GPIO 49
#define UART_LIGHT_CONTROLLER_RX_GPIO 50
#elif defined(CONFIG_UART_LIGHT_CONTROLLER_PRESET_PRODUCTION)
#define UART_LIGHT_CONTROLLER_PORT 0
#define UART_LIGHT_CONTROLLER_TX_GPIO 4
#define UART_LIGHT_CONTROLLER_RX_GPIO 5
#elif defined(CONFIG_UART_LIGHT_CONTROLLER_PRESET_CUSTOM)
#define UART_LIGHT_CONTROLLER_PORT CONFIG_UART_LIGHT_CONTROLLER_PORT
#define UART_LIGHT_CONTROLLER_TX_GPIO CONFIG_UART_LIGHT_CONTROLLER_TX_GPIO
#define UART_LIGHT_CONTROLLER_RX_GPIO CONFIG_UART_LIGHT_CONTROLLER_RX_GPIO
#else
#define UART_LIGHT_CONTROLLER_PORT 4
#define UART_LIGHT_CONTROLLER_TX_GPIO 49
#define UART_LIGHT_CONTROLLER_RX_GPIO 50
#endif

/** @brief Default warm LED brightness */
#define UART_LIGHT_CONTROLLER_DEFAULT_WARM CONFIG_UART_LIGHT_CONTROLLER_DEFAULT_WARM

/** @brief Default cold LED brightness */
#define UART_LIGHT_CONTROLLER_DEFAULT_COLD CONFIG_UART_LIGHT_CONTROLLER_DEFAULT_COLD

/** @brief Default red component */
#define UART_LIGHT_CONTROLLER_DEFAULT_RED CONFIG_UART_LIGHT_CONTROLLER_DEFAULT_RED

/** @brief Default green component */
#define UART_LIGHT_CONTROLLER_DEFAULT_GREEN CONFIG_UART_LIGHT_CONTROLLER_DEFAULT_GREEN

/** @brief Default blue component */
#define UART_LIGHT_CONTROLLER_DEFAULT_BLUE CONFIG_UART_LIGHT_CONTROLLER_DEFAULT_BLUE

#endif

#ifdef __cplusplus
}
#endif