/*
 * WiFi + WebSocket Demo for ESP32-P4 + C5 (WT01P4C5-S1)
 *
 * Demonstrates WiFi Manager and WebSocket Manager components.
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_manager.h"
#include "websocket_manager.h"

static const char *TAG = "main";

/* WebSocket data event callback */
static void ws_data_callback(ws_manager_event_t event, void *data)
{
    switch (event) {
    case WS_MANAGER_EVENT_CONNECTED:
        ESP_LOGI(TAG, "[WS Callback] Connected");
        break;
    case WS_MANAGER_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "[WS Callback] Disconnected");
        break;
    case WS_MANAGER_EVENT_DATA: {
        ws_manager_data_t *msg = (ws_manager_data_t *)data;
        if (msg && msg->data_len > 0 && msg->data_len < 256) {
            ESP_LOGI(TAG, "[WS Callback] Data received: type=%s len=%d client_fd=%d",
                     msg->type == WS_DATA_TYPE_TEXT ? "TEXT" : "BIN",
                     msg->data_len, msg->client_fd);
        }
        break;
    }
    case WS_MANAGER_EVENT_ERROR:
        ESP_LOGE(TAG, "[WS Callback] Error");
        break;
    case WS_MANAGER_EVENT_SERVER_CONNECT:
        ESP_LOGI(TAG, "[WS Callback] New server client connected");
        break;
    case WS_MANAGER_EVENT_SERVER_DISCONNECT:
        ESP_LOGI(TAG, "[WS Callback] Server client disconnected");
        break;
    }
}

/* WiFi event callback */
static void wifi_event_callback(wifi_manager_event_t event, void *data)
{
    switch (event) {
    case WIFI_MANAGER_EVENT_CONNECTED:
        ESP_LOGI(TAG, "[WiFi Callback] Connected");
        break;
    case WIFI_MANAGER_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "[WiFi Callback] Disconnected");
        break;
    case WIFI_MANAGER_EVENT_GOT_IP: {
        wifi_manager_info_t info;
        if (wifi_manager_get_info(&info) == ESP_OK) {
            ESP_LOGI(TAG, "[WiFi Callback] Got IP: %s", info.ip);
        }
        break;
    }
    case WIFI_MANAGER_EVENT_SCAN_DONE:
        ESP_LOGI(TAG, "[WiFi Callback] Scan done");
        break;
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== WiFi + WebSocket Demo for WT01P4C5-S1 ===");

    /* Step 1: Register WiFi callbacks and initialize */
    wifi_manager_register_handler(WIFI_MANAGER_EVENT_CONNECTED, wifi_event_callback);
    wifi_manager_register_handler(WIFI_MANAGER_EVENT_DISCONNECTED, wifi_event_callback);
    wifi_manager_register_handler(WIFI_MANAGER_EVENT_GOT_IP, wifi_event_callback);

    esp_err_t err = wifi_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi Manager init failed: %s", esp_err_to_name(err));
        return;
    }

    /* Wait for WiFi connection */
    while (!wifi_manager_is_connected()) {
        ESP_LOGI(TAG, "Waiting for WiFi connection...");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    /* Step 2: Initialize WebSocket Manager */
    ws_manager_register_handler(WS_MANAGER_EVENT_CONNECTED, ws_data_callback);
    ws_manager_register_handler(WS_MANAGER_EVENT_DISCONNECTED, ws_data_callback);
    ws_manager_register_handler(WS_MANAGER_EVENT_DATA, ws_data_callback);
    ws_manager_register_handler(WS_MANAGER_EVENT_ERROR, ws_data_callback);
    ws_manager_register_handler(WS_MANAGER_EVENT_SERVER_CONNECT, ws_data_callback);

    err = ws_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket Manager init failed: %s", esp_err_to_name(err));
        return;
    }

#if (WS_MANAGER_SERVER_ENABLE == 1)
    /* Step 3: Start WebSocket Server */
    err = ws_manager_server_start();
    if (err == ESP_OK) {
        wifi_manager_info_t wifi_info;
        if (wifi_manager_get_info(&wifi_info) == ESP_OK) {
            ESP_LOGI(TAG, "WebSocket server running at ws://%s:%d/ws",
                     wifi_info.ip, WS_MANAGER_SERVER_PORT);
        }
    }
#endif

#if (WS_MANAGER_CLIENT_ENABLE == 1)
    /* Step 4: Start WebSocket Client (configure URI via menuconfig or code) */
    ESP_LOGI(TAG, "WebSocket client ready. Call ws_manager_client_start() with a URI to connect.");
    ESP_LOGI(TAG, "Example: ws://echo.websocket.org");
#endif

    ESP_LOGI(TAG, "=== Setup complete ===");

    /* Main loop */
    int counter = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        counter++;

#if (WS_MANAGER_CLIENT_ENABLE == 1)
        if (ws_manager_client_is_connected()) {
            char send_buf[64];
            int len = snprintf(send_buf, sizeof(send_buf), "Hello #%d from ESP32-P4", counter);
            ws_manager_client_send_text(send_buf, len, 1000);
            ESP_LOGI(TAG, "Sent: %s", send_buf);
        }
#endif

#if (WS_MANAGER_SERVER_ENABLE == 1)
        if (ws_manager_server_is_running() && ws_manager_server_get_client_count() > 0) {
            char send_buf[64];
            int len = snprintf(send_buf, sizeof(send_buf), "Server push #%d", counter);
            ws_manager_server_broadcast_text(send_buf, len);
        }
#endif

        ESP_LOGI(TAG, "Heap: %" PRIu32 " bytes, WiFi: %s",
                 esp_get_free_heap_size(),
                 wifi_manager_is_connected() ? "up" : "down");
    }
}
