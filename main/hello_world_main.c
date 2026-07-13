/*
 * Smart Voice Assistant for ESP32-P4 + C5 (WT01P4C5-S1)
 *
 * Integrates WiFi Manager, WebSocket Manager, and Xiaozhi Voice Assistant.
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_manager.h"
#include "websocket_manager.h"
#include "xiaozhi_manager.h"

static const char *TAG = "MAIN";

/*---------------------------------------------------------------
 * WiFi event callback
 *-------------------------------------------------------------*/
static void wifi_event_callback(wifi_manager_event_t event, void *data)
{
    switch (event) {
    case WIFI_MANAGER_EVENT_CONNECTED:
        ESP_LOGI(TAG, "[WiFi] Connected");
        break;
    case WIFI_MANAGER_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "[WiFi] Disconnected");
        break;
    case WIFI_MANAGER_EVENT_GOT_IP: {
        wifi_manager_info_t info;
        if (wifi_manager_get_info(&info) == ESP_OK) {
            ESP_LOGI(TAG, "[WiFi] Got IP: %s", info.ip);
        }
        break;
    }
    case WIFI_MANAGER_EVENT_SCAN_DONE:
        ESP_LOGI(TAG, "[WiFi] Scan done");
        break;
    }
}

/*---------------------------------------------------------------
 * WebSocket event callback
 *-------------------------------------------------------------*/
static void ws_data_callback(ws_manager_event_t event, void *data)
{
    switch (event) {
    case WS_MANAGER_EVENT_CONNECTED:
        ESP_LOGI(TAG, "[WS] Connected");
        break;
    case WS_MANAGER_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "[WS] Disconnected");
        break;
    case WS_MANAGER_EVENT_DATA: {
        ws_manager_data_t *msg = (ws_manager_data_t *)data;
        if (msg && msg->data_len > 0 && msg->data_len < 256) {
            ESP_LOGI(TAG, "[WS] Data: type=%s len=%d",
                     msg->type == WS_DATA_TYPE_TEXT ? "TEXT" : "BIN",
                     msg->data_len);
        }
        break;
    }
    case WS_MANAGER_EVENT_ERROR:
        ESP_LOGE(TAG, "[WS] Error");
        break;
    case WS_MANAGER_EVENT_SERVER_CONNECT:
        ESP_LOGI(TAG, "[WS] Server client connected");
        break;
    case WS_MANAGER_EVENT_SERVER_DISCONNECT:
        ESP_LOGI(TAG, "[WS] Server client disconnected");
        break;
    }
}

/*---------------------------------------------------------------
 * Xiaozhi event callback
 *-------------------------------------------------------------*/
static void xiaozhi_event_callback(xiaozhi_manager_event_t event, void *data, void *ctx)
{
    switch (event) {
    case XIAOZHI_MANAGER_EVENT_CONNECTED:
        ESP_LOGI(TAG, "[Xiaozhi] Connected to server");
        break;
    case XIAOZHI_MANAGER_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "[Xiaozhi] Disconnected from server");
        break;
    case XIAOZHI_MANAGER_EVENT_AUDIO_CHANNEL_OPENED:
        ESP_LOGI(TAG, "[Xiaozhi] Audio channel opened");
        break;
    case XIAOZHI_MANAGER_EVENT_AUDIO_CHANNEL_CLOSED:
        ESP_LOGI(TAG, "[Xiaozhi] Audio channel closed");
        break;
    case XIAOZHI_MANAGER_EVENT_TTS_START:
        ESP_LOGI(TAG, "[Xiaozhi] TTS started");
        break;
    case XIAOZHI_MANAGER_EVENT_TTS_STOP:
        ESP_LOGI(TAG, "[Xiaozhi] TTS stopped");
        break;
    case XIAOZHI_MANAGER_EVENT_TTS_SENTENCE:
        ESP_LOGI(TAG, "[Xiaozhi] TTS sentence: %s", data ? (const char *)data : "");
        break;
    case XIAOZHI_MANAGER_EVENT_STT_TEXT:
        ESP_LOGI(TAG, "[Xiaozhi] STT text: %s", data ? (const char *)data : "");
        break;
    case XIAOZHI_MANAGER_EVENT_ERROR:
        ESP_LOGE(TAG, "[Xiaozhi] Error occurred");
        break;
    }
}

/*---------------------------------------------------------------
 * Xiaozhi audio callback (TTS audio data from server)
 * Currently logs audio data reception since no speaker hardware.
 * When audio hardware is available, this callback should feed
 * data to the I2S TX channel for playback.
 *-------------------------------------------------------------*/
static void xiaozhi_audio_callback(const uint8_t *data, int len, void *ctx)
{
    /* TODO: Feed data to I2S TX channel when audio hardware is available */
    ESP_LOGD(TAG, "[Xiaozhi] Audio data: %d bytes", len);
}

/*---------------------------------------------------------------
 * Application entry point
 *-------------------------------------------------------------*/
void app_main(void)
{
    ESP_LOGI(TAG, "=== Smart Voice Assistant for WT01P4C5-S1 ===");

    /* Step 1: Initialize WiFi Manager */
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

    /* Step 2: Initialize WebSocket Manager (independent of xiaozhi) */
    ws_manager_register_handler(WS_MANAGER_EVENT_CONNECTED, ws_data_callback);
    ws_manager_register_handler(WS_MANAGER_EVENT_DISCONNECTED, ws_data_callback);
    ws_manager_register_handler(WS_MANAGER_EVENT_DATA, ws_data_callback);
    ws_manager_register_handler(WS_MANAGER_EVENT_ERROR, ws_data_callback);

    err = ws_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket Manager init failed: %s", esp_err_to_name(err));
    }

#if (WS_MANAGER_SERVER_ENABLE == 1)
    err = ws_manager_server_start();
    if (err == ESP_OK) {
        wifi_manager_info_t wifi_info;
        if (wifi_manager_get_info(&wifi_info) == ESP_OK) {
            ESP_LOGI(TAG, "WebSocket server running at ws://%s:%d/ws",
                     wifi_info.ip, WS_MANAGER_SERVER_PORT);
        }
    }
#endif

    /* Step 3: Initialize Xiaozhi Manager */
    xiaozhi_manager_config_t xiaozhi_cfg = XIAOZHI_MANAGER_DEFAULT_CONFIG();
    xiaozhi_cfg.event_cb = xiaozhi_event_callback;
    xiaozhi_cfg.audio_cb = xiaozhi_audio_callback;

    err = xiaozhi_manager_init(&xiaozhi_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Xiaozhi Manager init failed: %s", esp_err_to_name(err));
    } else {
        /* Start xiaozhi chat session */
        err = xiaozhi_manager_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Xiaozhi Manager start failed: %s", esp_err_to_name(err));
        }
    }

    ESP_LOGI(TAG, "=== Setup complete ===");

    /* Main loop - monitor system health */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));

        xiaozhi_manager_state_t xiaozhi_state = xiaozhi_manager_get_state();
        ESP_LOGI(TAG, "Heap: %lu bytes | WiFi: %s | Xiaozhi: %d",
                 (unsigned long)esp_get_free_heap_size(),
                 wifi_manager_is_connected() ? "up" : "down",
                 xiaozhi_state);
    }
}
