/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_demos.h"
#include "board_config.h"
#include "board_init.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "simple_gui.h"

static const char *TAG = "main";

void app_main(void)
{
    // Step 1: Initialize LCD hardware (DSI PHY, backlight, DSI bus, ST7701S)
    esp_lcd_panel_handle_t panel_handle = NULL;
    board_init_lcd(&panel_handle);

    // Step 2: Initialize GUI with double-buffer support
    simple_gui_t gui;
    gui_init_double_buffer(&gui, panel_handle, BOARD_LCD_H_RES, BOARD_LCD_V_RES);

    // Step 3: Initialize touch controller (GT911 via I2C)
    esp_lcd_touch_handle_t tp_handle = NULL;
    esp_err_t ret                    = touch_init(&gui, &tp_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Touch init failed, continuing without touch");
    }

    // Step 4: Run the selected demo
    app_run_demo(&gui, tp_handle);

    // Step 5: Cleanup
    touch_deinit(tp_handle);

    ESP_LOGI(TAG, "Demo completed");

    // Keep running
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
