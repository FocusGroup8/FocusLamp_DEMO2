/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "esp_lcd_panel_dev.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief GC9107 LCD panel initialization structure
 */
typedef struct {
    esp_lcd_panel_dev_config_t base; /*!< Base panel device config */
    uint8_t madctl;                  /*!< MADCTL value (orientation) */
} esp_lcd_panel_gc9107_config_t;

/**
 * @brief Create GC9107 panel handle
 *
 * @param[in]  io          Panel IO handle created from esp_lcd_new_panel_io_spi()
 * @param[in]  panel_dev   Panel device config (reset_gpio_num, color depth, etc.)
 * @param[out] ret_panel   Returned panel handle
 * @return ESP_OK on success
 */
esp_err_t esp_lcd_new_panel_gc9107(esp_lcd_panel_io_handle_t io,
                                   const esp_lcd_panel_dev_config_t *panel_dev,
                                   esp_lcd_panel_handle_t *ret_panel);

#ifdef __cplusplus
}
#endif
