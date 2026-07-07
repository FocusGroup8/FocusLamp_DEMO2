/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_demos.h"

#include "board_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_EXAMPLE_ENABLE_TOUCH_GAME
#include "touch_game.h"
#endif
#if CONFIG_EXAMPLE_ENABLE_GESTURE_RECOGNITION
#include "gesture_recognition.h"
#endif
#include "gesture_data_collector.h"
#include "touch_gui.h"

static const char *TAG = "DEMOS";

void app_run_demo(simple_gui_t *gui, esp_lcd_touch_handle_t tp)
{
#if CONFIG_EXAMPLE_DEMO_TOUCH_GAME
    ESP_LOGI(TAG, "Running Touch Game Demo...");
    touch_game_demo(gui, tp);

#elif CONFIG_EXAMPLE_DEMO_GESTURE_RECOGNITION
    ESP_LOGI(TAG, "Running Gesture Recognition Demo...");
    gesture_recognition_demo(gui, tp);

#elif CONFIG_EXAMPLE_DEMO_TOUCH_GUI
    ESP_LOGI(TAG, "Running Touch GUI Demo...");
    touch_gui_demo(gui, tp);

#elif CONFIG_EXAMPLE_DEMO_DATA_COLLECTOR
    ESP_LOGI(TAG, "Running Gesture Data Collector...");
    gesture_data_collector_demo(gui, tp);

#else
    ESP_LOGW(TAG, "No demo selected in Kconfig");
    gui_clear_screen(gui, COLOR_BLACK);
    gui_draw_string(gui, 10, 200, "No demo selected", COLOR_WHITE, COLOR_BLACK, 2);
    gui_draw_string(gui, 10, 240, "Enable one in menuconfig", COLOR_CYAN, COLOR_BLACK, 1);
    gui_swap_buffers(gui);
    vTaskDelay(pdMS_TO_TICKS(5000));
#endif
}
