/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "led_test.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_controller.h"

static const char *TAG = "LED_TEST";

esp_err_t led_test_run(void)
{
    ESP_LOGI(TAG, "===== LED Controller Test Start =====");

    /* Step 1: Initialize */
    ESP_LOGI(TAG, "[1/5] Initializing LED controller...");
    esp_err_t ret = led_controller_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Step 2: Test LED_A (warm/暖光) at 50% */
    ESP_LOGI(TAG, "[2/5] Testing LED_A (warm/暖光) at 50%% for 2 seconds...");
    led_set_brightness(LED_CHANNEL_A, 50);
    vTaskDelay(pdMS_TO_TICKS(2000));
    led_set_brightness(LED_CHANNEL_A, 0);
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Step 3: Test LED_B (cool/冷光) at 50% */
    ESP_LOGI(TAG, "[3/5] Testing LED_B (cool/冷光) at 50%% for 2 seconds...");
    led_set_brightness(LED_CHANNEL_B, 50);
    vTaskDelay(pdMS_TO_TICKS(2000));
    led_set_brightness(LED_CHANNEL_B, 0);
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Step 4: Test both channels at 70% */
    ESP_LOGI(TAG, "[4/5] Testing both channels at 70%% for 2 seconds...");
    led_set_both_brightness(70, 70);
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* Step 5: Turn off */
    ESP_LOGI(TAG, "[5/5] Turning off...");
    led_off();

    ESP_LOGI(TAG, "===== LED Controller Test Complete =====");
    return ESP_OK;
}
