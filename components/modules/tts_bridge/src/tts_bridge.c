/*
 * tts_bridge.c - HTTP bridge to TTS board for voice broadcast
 *
 * Sends POST /api/tts/speak to the TTS board (10.10.40.92:80)
 * with JSON body: {"text": "播报内容", "priority": 1}
 *
 * The TTS board receives this and calls xiaozhi_manager_speak()
 * to play the audio through its speaker.
 */

#include "tts_bridge.h"
#include "tts_bridge_config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "TTS_BRIDGE";

/* JSON buffer size */
#define JSON_BUF_SIZE 512

/* WiFi 链路（ESP-Hosted）存在间歇性抖动，单次 POST 偶发失败；增加重试
 * 以提升提醒/播报注入可靠性。语音板 speak 已异步化，响应很快，超时无需
 * 保留旧的 15s（那是为旧同步 speak 预留的）。 */
#define TTS_BRIDGE_HTTP_TIMEOUT_MS 4000
#define TTS_BRIDGE_RETRY_COUNT 3
#define TTS_BRIDGE_RETRY_DELAY_MS 500

/*---------------------------------------------------------------
 * TTS Bridge: send text to TTS board for playback
 *-------------------------------------------------------------*/
esp_err_t tts_bridge_speak(const char *text)
{
    if (!text || strlen(text) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Build JSON: {"text": "...", "priority": 1} */
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGE(TAG, "Failed to create JSON object");
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "text", text);
    cJSON_AddNumberToObject(root, "priority", 1);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        ESP_LOGE(TAG, "Failed to print JSON");
        return ESP_ERR_NO_MEM;
    }

    /* Build URL */
    char url[128];
    snprintf(url, sizeof(url), "http://" TTS_BRIDGE_TARGET_IP "/api/tts/speak");

    ESP_LOGI(TAG, "Sending TTS: \"%s\" -> %s", text, url);

    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= TTS_BRIDGE_RETRY_COUNT; attempt++) {
        if (attempt > 1) {
            ESP_LOGW(TAG, "TTS bridge retry (%d/%d)", attempt, TTS_BRIDGE_RETRY_COUNT);
            vTaskDelay(pdMS_TO_TICKS(TTS_BRIDGE_RETRY_DELAY_MS));
        }

        /* Configure HTTP client */
        esp_http_client_config_t config = {
            .url = url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = TTS_BRIDGE_HTTP_TIMEOUT_MS,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            ESP_LOGE(TAG, "Failed to init HTTP client");
            continue;
        }

        /* Set headers */
        esp_http_client_set_header(client, "Content-Type", "application/json");

        /* Set POST body */
        esp_http_client_set_post_field(client, json_str, strlen(json_str));

        /* Perform request */
        err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            int status = esp_http_client_get_status_code(client);
            if (status == 200) {
                ESP_LOGI(TAG, "TTS response: HTTP %d (attempt %d)", status, attempt);
                esp_http_client_cleanup(client);
                free(json_str);
                return ESP_OK;
            }
            ESP_LOGW(TAG, "TTS HTTP %d (attempt %d)", status, attempt);
            err = ESP_FAIL;
        } else {
            ESP_LOGW(TAG, "HTTP POST failed (attempt %d): %s",
                     attempt, esp_err_to_name(err));
        }

        esp_http_client_cleanup(client);
    }

    free(json_str);

    return err;
}
