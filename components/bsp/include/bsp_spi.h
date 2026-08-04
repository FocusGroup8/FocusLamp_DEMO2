/*
 * bsp_spi.h - SPI initialization and communication for FocusLamp BSP
 */

#pragma once
#ifndef __BSP_SPI_H__
#define __BSP_SPI_H__

#include <stdint.h>
#include <stddef.h>
#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize SPI host for LCD:
 *        SCLK = GPIO36, MOSI = GPIO49, CS = GPIO48, DC = GPIO34
 *        Uses spi_bus_initialize + spi_bus_add_device.
 */
esp_err_t bsp_spi_init(void);

/**
 * @brief Transmit a buffer of bytes over SPI
 * @param data  Pointer to data buffer
 * @param len   Data length in bytes
 * @return esp_err_t
 */
esp_err_t bsp_spi_transmit_bytes(const uint8_t *data, size_t len);

/**
 * @brief Transmit a single byte over SPI
 * @param byte  Byte to transmit
 * @return esp_err_t
 */
esp_err_t bsp_spi_transmit_byte(uint8_t byte);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_SPI_H__ */