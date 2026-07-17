/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

#if CONFIG_EXAMPLE_ENABLE_DISPLAY
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "simple_gui.h"

/**
 * @brief Initialize all LCD hardware: DSI PHY power, backlight, DSI bus,
 *        DBI panel IO, DPI panel timing, and ST7701S driver.
 *
 * @param[out] out_panel  Handle to the created LCD panel
 */
void board_init_lcd(esp_lcd_panel_handle_t *out_panel);

/**
 * @brief Initialize I2C bus and GT911 touch controller.
 *
 * @param[in]  gui     GUI context (used for error display on failure)
 * @param[out] out_tp  Handle to the created touch panel
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t touch_init(simple_gui_t *gui, esp_lcd_touch_handle_t *out_tp);

/**
 * @brief Deinitialize touch controller and I2C bus.
 *
 * @param tp  Touch handle from touch_init()
 */
void touch_deinit(esp_lcd_touch_handle_t tp);
#endif

#if CONFIG_EXAMPLE_ENABLE_AUDIO
#include "audio/i2s_driver.h"

/**
 * @brief Initialize I2S audio subsystem (microphone + amplifier)
 *
 * @param[out] handles  I2S channel handles (TX + RX)
 * @return ESP_OK on success
 */
esp_err_t board_init_audio(i2s_audio_handles_t *handles);

/**
 * @brief Deinitialize I2S audio subsystem
 *
 * @param handles  I2S channel handles to release
 */
void board_deinit_audio(i2s_audio_handles_t *handles);
#endif

#if CONFIG_EXAMPLE_ENABLE_CAMERA
#include "esp_cam_sensor.h"

/**
 * @brief Initialize shared I2C0 bus (used by both Touch and Camera SCCB)
 *
 * Creates the I2C master bus if not already created. This function is
 * idempotent - calling it multiple times returns the same handle.
 *
 * @param[out] out_handle  I2C master bus handle
 * @return ESP_OK on success
 */
esp_err_t board_i2c_bus_init(i2c_master_bus_handle_t *out_handle);

/**
 * @brief Get the shared I2C0 bus handle (must call board_i2c_bus_init first)
 *
 * @return I2C master bus handle, or NULL if not initialized
 */
i2c_master_bus_handle_t board_i2c_bus_get_handle(void);

/**
 * @brief Release the shared I2C0 bus
 *
 * Only actually deletes the bus when reference count reaches zero.
 */
void board_i2c_bus_deinit(void);
#endif
