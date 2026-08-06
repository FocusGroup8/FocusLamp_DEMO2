/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tts_inject.h"

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "TTS_INJECT";

/* Target voice board IP:port, configured via menuconfig.
 * Fallback defaults keep compilation working before sdkconfig is regenerated. */
#ifndef CONFIG_TTS_INJECT_TARGET_IP
#define CONFIG_TTS_INJECT_TARGET_IP "192.168.1.110:80"
#endif

#define TTS_INJECT_HTTP_TIMEOUT_MS 2000

esp_err_t tts_inject_speak(const char *text)
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

    char url[160];
    snprintf(url, sizeof(url), "http://" CONFIG_TTS_INJECT_TARGET_IP "/api/tts/speak");

    ESP_LOGI(TAG, "Sending TTS: \"%s\" -> %s", text, url);

    esp_http_client_config_t config = {
        .url        = url,
        .method     = HTTP_METHOD_POST,
        .timeout_ms = TTS_INJECT_HTTP_TIMEOUT_MS,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        free(json_str);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_str, strlen(json_str));

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "POST /api/tts/speak failed: %s", esp_err_to_name(err));
    } else {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "TTS inject response: HTTP %d", status);
        if (status != 200) {
            err = ESP_FAIL;
        }
    }

    esp_http_client_cleanup(client);
    free(json_str);
    return err;
}
