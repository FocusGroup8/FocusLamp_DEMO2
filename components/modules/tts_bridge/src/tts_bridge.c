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

static const char *TAG = "TTS_BRIDGE";

/* JSON buffer size */
#define JSON_BUF_SIZE 512

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

    /* Configure HTTP client */
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        /* Voice board handles /api/tts/speak synchronously: when busy it
         * aborts the current TTS (up to 3s wait) and opens the audio channel
         * (up to 3s wait). 5s was too tight and caused ESP_ERR_HTTP_EAGAIN
         * timeouts even though the voice board received and played the text. */
        .timeout_ms = 15000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        free(json_str);
        return ESP_ERR_NO_MEM;
    }

    /* Set headers */
    esp_http_client_set_header(client, "Content-Type", "application/json");

    /* Set POST body */
    esp_http_client_set_post_field(client, json_str, strlen(json_str));

    /* Perform request */
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP POST failed: %s", esp_err_to_name(err));
    } else {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "TTS response: HTTP %d", status);
        if (status != 200) {
            err = ESP_FAIL;
        }
    }

    esp_http_client_cleanup(client);
    free(json_str);

    return err;
}
