/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "board_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network_manager.h"
#include "system_manager.h"

#if CONFIG_EXAMPLE_ENABLE_DISPLAY
#include "app_demos.h"
#include "board_init.h"
#include "simple_gui.h"
#endif

#if CONFIG_EXAMPLE_ENABLE_LED
#include "led_controller.h"
#include "led_test.h"
#endif

#if CONFIG_EXAMPLE_ENABLE_CAMERA
#include "camera_controller.h"
#include "camera_stream.h"
#endif

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32-P4 MIPI DSI Board System ===");
    ESP_LOGI(TAG, "Board type configured via Kconfig");

    esp_err_t ret = ESP_OK;

#if CONFIG_EXAMPLE_ENABLE_LED
    // Initialize LED before starting subsystem (display start is blocking)
    ESP_LOGI(TAG, "Initializing LED controller...");
    ret = led_controller_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LED init failed: %s", esp_err_to_name(ret));
    }
#endif

    // Initialize system manager (reads Kconfig and initializes selected subsystem)
    ret = system_manager_init(NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "System manager initialization failed: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "Check Kconfig configuration!");
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        return;
    }

#if CONFIG_EXAMPLE_RUN_LED_TEST
    // LED test runs before blocking display demo
    ESP_LOGI(TAG, "Running LED test...");
    led_test_run();
    ESP_LOGI(TAG, "LED test completed");
#endif

#if CONFIG_EXAMPLE_RUN_CAMERA_TEST
    // Camera test uses system_manager's already-initialized camera handles
    ESP_LOGI(TAG, "Running Camera test...");
    camera_handles_t *cam_handles = system_manager_get_camera_handles();
    if (cam_handles != NULL) {
        ret = camera_test_run(cam_handles);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Camera test failed: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "Camera test completed successfully");
        }
    } else {
        ESP_LOGE(TAG, "Camera not initialized, cannot run test");
    }
#endif

    // Start the configured subsystem (audio or display)
    // Note: display_system_start() with touch game demo is blocking
    ret = system_manager_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "System start failed: %s", esp_err_to_name(ret));
        system_manager_deinit();
        return;
    }

    ESP_LOGI(TAG, "System running on %s board",
             system_manager_is_audio_active() ? "BOTTOM (Audio)" : "TOP (Display/LED/Camera)");

    // Initialize network subsystem (WiFi + WebSocket + MCP + camera stream)
    // Note: network_manager_init blocks until WiFi connects (up to 30s timeout)
    ESP_LOGI(TAG, "Initializing network subsystem...");
    ret = network_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Network init failed: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "Camera streaming will not be available. Check WiFi config.");
    } else {
        ESP_LOGI(TAG, "Network ready. IP: %s", network_manager_get_ip());
#if CONFIG_EXAMPLE_ENABLE_CAMERA
        // Auto-start camera streaming after network is up
        ret = network_manager_start_camera_stream();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Camera stream start failed: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "Camera streaming started. Connect with browser:");
            ESP_LOGI(TAG, "  HTML UI: open tools/web_ui/index.html");
            ESP_LOGI(TAG, "  Camera stream: ws://%s/camera", network_manager_get_ip());
            ESP_LOGI(TAG, "  MCP control: ws://%s/mcp", network_manager_get_ip());
        }
#endif
    }

    // Keep running
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Cleanup (will never reach here in normal operation)
    network_manager_deinit();
#if CONFIG_EXAMPLE_ENABLE_LED
    led_controller_deinit();
#endif
    system_manager_stop();
    system_manager_deinit();
    ESP_LOGI(TAG, "System shutdown complete");
}
