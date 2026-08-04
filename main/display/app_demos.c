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

#if CONFIG_EXAMPLE_ENABLE_AUDIO
#include "audio/audio_test.h"
#include "audio/i2s_test.h"
#endif

static const char *TAG = "DEMOS";

#if CONFIG_EXAMPLE_RUN_I2S_TEST
static void audio_rec_test_task(void *arg)
{
    ESP_LOGI(TAG, "Starting Audio Recording-to-Playback test...");
    esp_err_t ret = audio_test_rec_to_play();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Audio test completed successfully");
    } else {
        ESP_LOGE(TAG, "Audio test failed: %s", esp_err_to_name(ret));
    }
    vTaskDelete(NULL);
}
#endif

void app_run_demo(simple_gui_t *gui, esp_lcd_touch_handle_t tp)
{
#if CONFIG_EXAMPLE_RUN_I2S_TEST
    // Run Audio Recording-to-Playback test
    ESP_LOGI(TAG, "Launching Audio Recording-to-Playback test task...");
    xTaskCreate(audio_rec_test_task, "audio_rec_test", 16384, NULL, 5, NULL); // Increased stack for recorder + player
    vTaskDelay(pdMS_TO_TICKS(35000)); // Wait for tests to complete (30s recording + 30s playback + 5s overhead)
#endif

    // Skip GUI demos when running in I2S test mode (gui == NULL)
    if (gui == NULL) {
        ESP_LOGI(TAG, "No GUI handle available, skipping GUI demos");
        return;
    }

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
