/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "driver/i2s_std.h"
#include "esp_err.h"

// C++ compatibility
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief I2S audio channel handles for full-duplex operation
 */
typedef struct {
    i2s_chan_handle_t tx_handle; /*!< I2S TX channel (→ amplifier) */
    i2s_chan_handle_t rx_handle; /*!< I2S RX channel (← microphone) */
} i2s_audio_handles_t;

/**
 * @brief Initialize I2S in full-duplex mode
 *
 * Creates TX and RX channels on the same I2S controller.
 * Both channels share BCLK and WS pins, with separate data pins.
 * Channels are created but NOT enabled after this call.
 *
 * @param[out] handles  Pointer to store the I2S channel handles
 * @return ESP_OK on success
 */
esp_err_t i2s_audio_init(i2s_audio_handles_t *handles);

/**
 * @brief Enable TX and RX channels
 */
esp_err_t i2s_audio_enable(const i2s_audio_handles_t *handles);

/**
 * @brief Disable TX and RX channels
 */
esp_err_t i2s_audio_disable(const i2s_audio_handles_t *handles);

/**
 * @brief Write PCM data to I2S TX channel (blocking)
 *
 * @param handles   I2S handles
 * @param data      PCM sample data (16-bit signed)
 * @param len       Data length in bytes
 * @param[out] bytes_written  Actual bytes written
 * @param timeout_ms  Timeout in ms (portMAX_DELAY for infinite)
 * @return ESP_OK on success
 */
esp_err_t i2s_audio_write(const i2s_audio_handles_t *handles, const void *data, size_t len, size_t *bytes_written,
                          uint32_t timeout_ms);

/**
 * @brief Read PCM data from I2S RX channel (blocking)
 *
 * @param handles   I2S handles
 * @param[out] data  Buffer for received PCM data
 * @param len       Buffer length in bytes
 * @param[out] bytes_read  Actual bytes read
 * @param timeout_ms  Timeout in ms (portMAX_DELAY for infinite)
 * @return ESP_OK on success
 */
esp_err_t i2s_audio_read(const i2s_audio_handles_t *handles, void *data, size_t len, size_t *bytes_read,
                         uint32_t timeout_ms);

/**
 * @brief Deinitialize I2S and release resources
 */
void i2s_audio_deinit(i2s_audio_handles_t *handles);

#ifdef __cplusplus
}
#endif