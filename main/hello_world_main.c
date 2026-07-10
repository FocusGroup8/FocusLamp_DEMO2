/*
 * WiFi Remote STA for ESP32-P4 + C5 (WT01P4C5-S1)
 *
 * Uses wifi_manager component for modular WiFi management.
 */

#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_manager.h"

static const char *TAG = "main";

/* WiFi Manager event callback */
static void wifi_event_callback(wifi_manager_event_t event, void *data)
{
    switch (event) {
    case WIFI_MANAGER_EVENT_CONNECTED:
        ESP_LOGI(TAG, "[Callback] WiFi connected");
        break;
    case WIFI_MANAGER_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "[Callback] WiFi disconnected");
        break;
    case WIFI_MANAGER_EVENT_GOT_IP: {
        wifi_manager_info_t info;
        if (wifi_manager_get_info(&info) == ESP_OK) {
            ESP_LOGI(TAG, "[Callback] Got IP: %s", info.ip);
        }
        break;
    }
    case WIFI_MANAGER_EVENT_SCAN_DONE:
        ESP_LOGI(TAG, "[Callback] WiFi scan done");
        break;
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== WiFi Remote STA for WT01P4C5-S1 ===");
    ESP_LOGI(TAG, "Host: ESP32-P4 | Co-processor: ESP32-C5");
    ESP_LOGI(TAG, "Transport: SDIO (CLK=18, CMD=19, D0=14, D1=15, D2=16, D3=17)");

    /* Step 1: Register WiFi event callback */
    wifi_manager_register_handler(WIFI_MANAGER_EVENT_CONNECTED, wifi_event_callback);
    wifi_manager_register_handler(WIFI_MANAGER_EVENT_DISCONNECTED, wifi_event_callback);
    wifi_manager_register_handler(WIFI_MANAGER_EVENT_GOT_IP, wifi_event_callback);
    wifi_manager_register_handler(WIFI_MANAGER_EVENT_SCAN_DONE, wifi_event_callback);

    /* Step 2: Initialize WiFi Manager (includes NVS, Hosted, WiFi) */
    esp_err_t err = wifi_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi Manager init failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "WiFi Manager initialized");

    /* Step 3: Print connection info */
    if (wifi_manager_is_connected()) {
        wifi_manager_info_t info;
        if (wifi_manager_get_info(&info) == ESP_OK) {
            ESP_LOGI(TAG, "Connected AP info:");
            ESP_LOGI(TAG, "  SSID: %s", info.ssid);
            ESP_LOGI(TAG, "  IP: %s", info.ip);
            ESP_LOGI(TAG, "  RSSI: %d dBm", info.rssi);
            ESP_LOGI(TAG, "  Channel: %d", info.channel);
        }
    }

    /* Step 4: Perform WiFi scan */
    ESP_LOGI(TAG, "Starting WiFi scan...");
    wifi_manager_ap_info_t ap_records[20];
    uint16_t ap_count = 20;
    err = wifi_manager_scan(ap_records, &ap_count);
    if (err == ESP_OK) {
        for (int i = 0; i < ap_count; i++) {
            ESP_LOGI(TAG, "  [%d] SSID:%-32s RSSI:%d Ch:%d Auth:%d",
                     i + 1, ap_records[i].ssid, ap_records[i].rssi,
                     ap_records[i].channel, ap_records[i].authmode);
        }
    }

    ESP_LOGI(TAG, "=== WiFi Manager demo complete ===");

    /* Main loop - monitor system health */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ESP_LOGI(TAG, "System running, free heap: %" PRIu32 " bytes, WiFi: %s",
                 esp_get_free_heap_size(),
                 wifi_manager_is_connected() ? "connected" : "disconnected");
    }
}
