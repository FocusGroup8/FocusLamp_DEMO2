/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tts_inject.h"

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "focuslamp_discovery.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "TTS_INJECT";

#define TTS_INJECT_HTTP_TIMEOUT_MS 2000
/* WiFi 链路（ESP-Hosted）存在间歇性抖动，单次 POST 偶发失败。
 * 增加重试以提升欢迎语/提醒注入可靠性（临时诊断期间观察值）。 */
#define TTS_INJECT_RETRY_COUNT 3
#define TTS_INJECT_RETRY_DELAY_MS 300
#define TTS_INJECT_RESPONSE_CAPACITY 256

typedef struct {
    char body[TTS_INJECT_RESPONSE_CAPACITY];
    size_t length;
    bool overflow;
} tts_inject_response_t;

/* perform() delivers both fixed-length and chunked response bodies here. */
static esp_err_t tts_inject_http_event(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }

    tts_inject_response_t *response = event->user_data;
    if (!response || response->overflow) {
        return ESP_OK;
    }
    size_t length = (size_t)event->data_len;
    if (length > sizeof(response->body) - 1 - response->length) {
        response->overflow = true;
        return ESP_OK;  /* Drain the response; do not turn rejection into a retry. */
    }
    memcpy(response->body + response->length, event->data, length);
    response->length += length;
    response->body[response->length] = '\0';
    return ESP_OK;
}

static bool tts_inject_response_accepted(const tts_inject_response_t *response)
{
    if (response->overflow || response->length == 0 ||
        strlen(response->body) != response->length) {
        return false;
    }

    /* Reject trailing garbage, embedded NULs, and non-boolean "ok" fields. */
    cJSON *root = cJSON_ParseWithLengthOpts(response->body,
                                          response->length + 1, NULL, true);
    if (!root) {
        return false;
    }
    const cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    bool accepted = cJSON_IsObject(root) && cJSON_IsBool(ok) && cJSON_IsTrue(ok);
    cJSON_Delete(root);
    return accepted;
}

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

    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= TTS_INJECT_RETRY_COUNT; attempt++) {
        if (attempt > 1) {
            vTaskDelay(pdMS_TO_TICKS(TTS_INJECT_RETRY_DELAY_MS));
        }

        focuslamp_endpoint_t peer;
        if (focuslamp_discovery_get_peer(FOCUSLAMP_ROLE_CONTROLLER, &peer) != ESP_OK) {
            err = ESP_ERR_NOT_FOUND;
            break;
        }

        char url[160];
        snprintf(url, sizeof(url), "http://%s:%u/api/tts/speak", peer.host, (unsigned)peer.port);
        if (attempt == 1) {
            ESP_LOGI(TAG, "Sending TTS: \"%s\" -> %s", text, url);
        } else {
            ESP_LOGW(TAG, "TTS inject retry (%d/%d) -> %s", attempt, TTS_INJECT_RETRY_COUNT, url);
        }

        tts_inject_response_t response = {0};
        esp_http_client_config_t config = {
            .url        = url,
            .method     = HTTP_METHOD_POST,
            .timeout_ms = TTS_INJECT_HTTP_TIMEOUT_MS,
            .event_handler = tts_inject_http_event,
            .user_data = &response,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            ESP_LOGE(TAG, "Failed to init HTTP client");
            continue;
        }

        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, json_str, strlen(json_str));

        err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            int status = esp_http_client_get_status_code(client);
            if (status == 200) {
                if (tts_inject_response_accepted(&response)) {
                    ESP_LOGI(TAG, "TTS request accepted (queued, not playback confirmation; attempt %d)", attempt);
                } else {
                    ESP_LOGW(TAG, "TTS request not accepted: invalid/rejected response (bytes=%u overflow=%d)",
                             (unsigned)response.length, response.overflow);
                    err = ESP_FAIL;
                }
                esp_http_client_cleanup(client);
                break;  /* An HTTP 200 business result must not be auto-retried. */
            }
            ESP_LOGW(TAG, "TTS inject HTTP %d (attempt %d)", status, attempt);
            err = ESP_FAIL;
        } else {
            ESP_LOGW(TAG, "POST /api/tts/speak failed (attempt %d): %s", attempt, esp_err_to_name(err));
        }

        esp_http_client_cleanup(client);
    }

    free(json_str);
    return err;
}

esp_err_t tts_inject_end_chat(void)
{
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= TTS_INJECT_RETRY_COUNT; attempt++) {
        if (attempt > 1) {
            vTaskDelay(pdMS_TO_TICKS(TTS_INJECT_RETRY_DELAY_MS));
        }

        focuslamp_endpoint_t peer;
        if (focuslamp_discovery_get_peer(FOCUSLAMP_ROLE_CONTROLLER, &peer) != ESP_OK) {
            err = ESP_ERR_NOT_FOUND;
            break;
        }

        char url[160];
        snprintf(url, sizeof(url), "http://%s:%u/api/chat/end", peer.host, (unsigned)peer.port);
        if (attempt == 1) {
            ESP_LOGI(TAG, "Sending end-chat -> %s", url);
        } else {
            ESP_LOGW(TAG, "End-chat inject retry (%d/%d) -> %s", attempt, TTS_INJECT_RETRY_COUNT, url);
        }

        esp_http_client_config_t config = {
            .url        = url,
            .method     = HTTP_METHOD_POST,
            .timeout_ms = TTS_INJECT_HTTP_TIMEOUT_MS,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            ESP_LOGE(TAG, "Failed to init HTTP client");
            continue;
        }

        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, "{}", 2);

        err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            int status = esp_http_client_get_status_code(client);
            if (status == 200) {
                ESP_LOGI(TAG, "End-chat response: HTTP %d (attempt %d)", status, attempt);
                esp_http_client_cleanup(client);
                return ESP_OK;
            }
            ESP_LOGW(TAG, "End-chat HTTP %d (attempt %d)", status, attempt);
            err = ESP_FAIL;
        } else {
            ESP_LOGW(TAG, "POST /api/chat/end failed (attempt %d): %s", attempt, esp_err_to_name(err));
        }

        esp_http_client_cleanup(client);
    }

    return err;
}
