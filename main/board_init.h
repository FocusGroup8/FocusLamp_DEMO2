/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "simple_gui.h"

#if CONFIG_EXAMPLE_ENABLE_AUDIO
#include "audio/i2s_driver.h"
#endif

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

#if CONFIG_EXAMPLE_ENABLE_AUDIO
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
