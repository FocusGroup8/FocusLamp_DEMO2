/*
 * bsp_uart.h - UART initialization and communication for FocusLamp BSP
 */

#pragma once
#ifndef __BSP_UART_H__
#define __BSP_UART_H__

#include <stdint.h>
#include <stddef.h>
#include "driver/uart.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize three UARTs:
 *        - UART1: EM3 servo (TX=GPIO8, RX=GPIO2)
 *        - UART2: LX servo  (TX=GPIO3, RX=GPIO20)
 *        - UART3: Dual-board communication (TX=GPIO54, RX=GPIO53)
 *        Baud rates and other parameters are read from system_config.h.
 */
esp_err_t bsp_uart_init(void);

/**
 * @brief Write bytes to a UART port
 * @param uart_port  UART port number
 * @param data       Pointer to data buffer
 * @param len        Data length in bytes
 * @return esp_err_t
 */
esp_err_t bsp_uart_write_bytes(uart_port_t uart_port, const uint8_t *data, size_t len);

/**
 * @brief Read bytes from a UART port
 * @param uart_port  UART port number
 * @param data       Pointer to receive buffer
 * @param len        Expected data length in bytes
 * @param timeout    Timeout in ticks (pdMS_TO_TICKS)
 * @return int       Number of bytes read, or -1 on error
 */
int bsp_uart_read_bytes(uart_port_t uart_port, uint8_t *data, size_t len, TickType_t timeout);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_UART_H__ */