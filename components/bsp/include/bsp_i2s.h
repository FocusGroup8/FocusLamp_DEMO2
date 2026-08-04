/*
 * bsp_i2s.h - I2S initialization and communication for FocusLamp BSP
 */

#pragma once
#ifndef __BSP_I2S_H__
#define __BSP_I2S_H__

#include <stdint.h>
#include <stddef.h>
#include <freertos/FreeRTOS.h>
#include "driver/i2s_std.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize I2S for audio:
 *        LRC = GPIO33, BCLK = GPIO32, DIN = GPIO31, SD = GPIO30
 *        Standard Philips format, sample rate from system_config.h.
 *        Uses i2s_driver_install + i2s_set_pin.
 */
esp_err_t bsp_i2s_init(void);

/**
 * @brief Write audio data over I2S
 * @param data  Pointer to PCM data buffer
 * @param len   Data length in bytes
 * @return esp_err_t
 */
esp_err_t bsp_i2s_write(const uint8_t *data, size_t len);

/**
 * @brief Read audio data over I2S
 * @param data     Pointer to receive buffer
 * @param len      Buffer size in bytes
 * @param timeout  Timeout in ticks
 * @return int     Number of bytes read, or -1 on error
 */
int bsp_i2s_read(uint8_t *data, size_t len, TickType_t timeout);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_I2S_H__ */