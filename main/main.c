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
#include "system_manager.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32-P4 MIPI DSI Audio/Display System ===");
    ESP_LOGI(TAG, "System mode configured via Kconfig");

    // Initialize system manager (reads Kconfig and initializes selected subsystem)
    esp_err_t ret = system_manager_init(NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "System manager initialization failed: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "Check Kconfig configuration!");
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        return;
    }

    // Start the configured subsystem (audio or display)
    ret = system_manager_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "System start failed: %s", esp_err_to_name(ret));
        system_manager_deinit();
        return;
    }

    ESP_LOGI(TAG, "System running in %s mode", system_manager_is_audio_active() ? "AUDIO" : "DISPLAY");

    // Keep running
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Cleanup (will never reach here in normal operation)
    system_manager_stop();
    system_manager_deinit();
    ESP_LOGI(TAG, "System shutdown complete");
}