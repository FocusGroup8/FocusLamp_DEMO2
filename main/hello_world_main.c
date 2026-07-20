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
#include "audio_bridge.h"
#include "wake_word_engine.h"

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
 * Wake word detection callback
 * Called by wake_word_engine when a custom wake word is detected.
 * Triggers xiaozhi_manager to start a conversation.
 *-------------------------------------------------------------*/
static void wake_word_detect_callback(int command_id, const char *command_str,
                                      wake_word_lang_t lang, float prob, void *ctx)
{
    ESP_LOGI(TAG, "[WakeWord] Detected: id=%d, cmd='%s', lang=%s, prob=%.2f",
             command_id, command_str ? command_str : "null",
             lang == WAKE_WORD_LANG_CN ? "CN" : "EN", prob);

    /* Trigger xiaozhi conversation with the detected wake word */
    esp_err_t err = xiaozhi_manager_send_wake_word(command_str ? command_str : "你好小智");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[WakeWord] Failed to trigger xiaozhi: %s", esp_err_to_name(err));
    }
}

/*---------------------------------------------------------------
 * Xiaozhi event callback
 *-------------------------------------------------------------*/
static void xiaozhi_event_callback(xiaozhi_manager_event_t event, void *data, void *ctx)
{
    switch (event) {
    case XIAOZHI_MANAGER_EVENT_CONNECTED:
        ESP_LOGI(TAG, "[Xiaozhi] Connected to server — waiting for wake word");
        /* No longer auto-opens audio channel or sends wake word here.
         * Wake word is triggered only by ESP-SR detection or button press,
         * via xiaozhi_manager_send_wake_word(). */
        break;
    case XIAOZHI_MANAGER_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "[Xiaozhi] Disconnected from server");
        break;
    case XIAOZHI_MANAGER_EVENT_AUDIO_CHANNEL_OPENED:
        ESP_LOGI(TAG, "[Xiaozhi] Audio channel opened");
        /* No longer auto-sends wake word here.
         * The wake word should already have been sent by the trigger source
         * (ESP-SR or button) that opened the audio channel. */
        break;
    case XIAOZHI_MANAGER_EVENT_AUDIO_CHANNEL_CLOSED:
        ESP_LOGI(TAG, "[Xiaozhi] Audio channel closed — back to idle");
        break;
    case XIAOZHI_MANAGER_EVENT_TTS_START:
        ESP_LOGI(TAG, "[Xiaozhi] TTS started");
        break;
    case XIAOZHI_MANAGER_EVENT_TTS_STOP:
        ESP_LOGI(TAG, "[Xiaozhi] TTS stopped — waiting for next wake word");
        break;
    case XIAOZHI_MANAGER_EVENT_TTS_SENTENCE:
        ESP_LOGI(TAG, "[Xiaozhi] TTS sentence: %s", data ? (const char *)data : "");
        break;
    case XIAOZHI_MANAGER_EVENT_STT_TEXT:
        ESP_LOGI(TAG, "[Xiaozhi] STT text: %s", data ? (const char *)data : "");
        break;
    case XIAOZHI_MANAGER_EVENT_SERVER_GOODBYE:
        ESP_LOGI(TAG, "[Xiaozhi] Server goodbye — conversation ended, waiting for wake word");
        break;
    case XIAOZHI_MANAGER_EVENT_ERROR:
        ESP_LOGE(TAG, "[Xiaozhi] Error occurred");
        break;
    }
}

/*---------------------------------------------------------------
 * Xiaozhi audio callback (TTS audio data from server)
 * Routes OPUS-encoded TTS audio to I2S speaker via audio_bridge.
 * When audio hardware is available, audio_bridge_tts_callback
 * decodes OPUS and writes PCM to I2S TX channel.
 *-------------------------------------------------------------*/
static void xiaozhi_audio_callback(const uint8_t *data, int len, void *ctx)
{
    audio_bridge_tts_callback(data, len, ctx);
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

    /* Step 3: Initialize Audio Bridge (I2S for TTS playback) */
    audio_bridge_config_t audio_cfg = AUDIO_BRIDGE_DEFAULT_CONFIG();
    err = audio_bridge_init(&audio_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Audio Bridge init failed: %s", esp_err_to_name(err));
    }

    /* Step 4: Initialize Xiaozhi Manager (init only — do NOT start yet) */
    xiaozhi_manager_config_t xiaozhi_cfg = XIAOZHI_MANAGER_DEFAULT_CONFIG();
    xiaozhi_cfg.event_cb = xiaozhi_event_callback;
    xiaozhi_cfg.audio_cb = xiaozhi_audio_callback;

    err = xiaozhi_manager_init(&xiaozhi_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Xiaozhi Manager init failed: %s", esp_err_to_name(err));
    }

    /* Step 5: Initialize Wake Word Engine BEFORE xiaozhi_manager_start()
     * so PCM callback is registered before mic_task starts running. */
    wake_word_engine_config_t wake_cfg = WAKE_WORD_ENGINE_DEFAULT_CONFIG();
#if defined(CONFIG_SR_MN_EN_MULTINET7_QUANT)
    wake_cfg.default_lang = WAKE_WORD_LANG_EN;
#else
    wake_cfg.default_lang = WAKE_WORD_LANG_CN;
#endif
    wake_cfg.detect_cb = wake_word_detect_callback;
    wake_cfg.det_timeout_ms = 2000;

    err = wake_word_engine_init(&wake_cfg);
    if (err == ESP_OK) {
        /* Register custom wake word commands based on compiled model */
#if defined(CONFIG_SR_MN_EN_MULTINET7_QUANT)
        wake_word_engine_add_command(1, "focus");
#elif defined(CONFIG_SR_MN_CN_MULTINET7_QUANT)
        wake_word_engine_add_command(1, "ni hao xiao zhi");  /* 你好小智 */
        wake_word_engine_add_command(2, "xiao zhi xiao zhi"); /* 小智小智 */
#endif
        void *cmd_err = wake_word_engine_update_commands();
        if (cmd_err != NULL) {
            ESP_LOGW(TAG, "Some wake word commands could not be parsed");
            /* Error details are logged internally by wake_word_engine.
             * The returned pointer must be freed via wake_word_engine_free_error(). */
            wake_word_engine_free_error(cmd_err);
        }

        /* Start wake word detection */
        err = wake_word_engine_start();
        if (err == ESP_OK) {
#if defined(CONFIG_SR_MN_EN_MULTINET7_QUANT)
            ESP_LOGI(TAG, "Wake word engine started — listening for 'focus'");
#elif defined(CONFIG_SR_MN_CN_MULTINET7_QUANT)
            ESP_LOGI(TAG, "Wake word engine started — listening for 'ni hao xiao zhi', 'xiao zhi xiao zhi'");
#endif
        } else {
            ESP_LOGE(TAG, "Wake word engine start failed: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "Wake Word Engine init failed: %s (model partition missing?)",
                 esp_err_to_name(err));
    }

    /* Step 6: Start Xiaozhi Manager — this starts mic_task which feeds PCM
     * to wake_word_engine via the callback registered in Step 5. */
    err = xiaozhi_manager_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Xiaozhi Manager start failed: %s", esp_err_to_name(err));
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
