/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "base_bridge.h"

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "BASE_BRIDGE";

/* Target base board IP:port, configured via menuconfig.
 * Fallback defaults keep compilation working before sdkconfig is regenerated. */
#ifndef CONFIG_BASE_BRIDGE_TARGET_IP
#define CONFIG_BASE_BRIDGE_TARGET_IP "192.168.1.120:80"
#endif

#define BRIDGE_HTTP_TIMEOUT_MS 2000
/* Shorter timeout for ambient query: when the base board is offline we want
 * the touch flow to fall back to the default brightness quickly instead of
 * blocking the action task for 2 seconds. */
#define AMBIENT_HTTP_TIMEOUT_MS 1000
#define RESP_BUF_SIZE 256

/*---------------------------------------------------------------
 * Common HTTP POST helper
 *-------------------------------------------------------------*/
static esp_err_t bridge_post_json(const char *path, const char *json_body)
{
    char url[160];
    snprintf(url, sizeof(url), "http://" CONFIG_BASE_BRIDGE_TARGET_IP "%s", path);

    esp_http_client_config_t config = {
        .url        = url,
        .method     = HTTP_METHOD_POST,
        .timeout_ms = BRIDGE_HTTP_TIMEOUT_MS,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_body, strlen(json_body));

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "POST %s failed: %s", path, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "POST %s returned HTTP %d", path, status);
        err = ESP_FAIL;
    } else {
        ESP_LOGI(TAG, "POST %s OK (HTTP %d)", path, status);
    }

    esp_http_client_cleanup(client);
    return err;
}

/*---------------------------------------------------------------
 * GET /api/ambient response body collector
 *-------------------------------------------------------------*/
typedef struct {
    char buf[RESP_BUF_SIZE];
    int len;
} ambient_resp_t;

static esp_err_t ambient_event_handler(esp_http_client_event_t *evt)
{
    ambient_resp_t *ctx = (ambient_resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int avail = (int)sizeof(ctx->buf) - ctx->len - 1;
        if (avail > evt->data_len) {
            avail = evt->data_len;
        }
        if (avail > 0) {
            memcpy(ctx->buf + ctx->len, evt->data, avail);
            ctx->len += avail;
            ctx->buf[ctx->len] = '\0';
        }
    }
    return ESP_OK;
}

/*---------------------------------------------------------------
 * Public API
 *-------------------------------------------------------------*/
esp_err_t base_bridge_get_ambient(int *level_out)
{
    if (!level_out) {
        return ESP_ERR_INVALID_ARG;
    }

    char url[160];
    snprintf(url, sizeof(url), "http://" CONFIG_BASE_BRIDGE_TARGET_IP "/api/ambient");

    ambient_resp_t resp = {0};
    esp_http_client_config_t config = {
        .url           = url,
        .method        = HTTP_METHOD_GET,
        .timeout_ms    = AMBIENT_HTTP_TIMEOUT_MS,
        .event_handler = ambient_event_handler,
        .user_data     = &resp,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GET /api/ambient failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (status != 200) {
        ESP_LOGW(TAG, "GET /api/ambient returned HTTP %d", status);
        return ESP_FAIL;
    }

    /* Parse {"ok":true,"level":N} */
    cJSON *root = cJSON_Parse(resp.buf);
    if (!root) {
        ESP_LOGW(TAG, "GET /api/ambient: invalid JSON: %s", resp.buf);
        return ESP_FAIL;
    }
    cJSON *level = cJSON_GetObjectItem(root, "level");
    if (!cJSON_IsNumber(level)) {
        ESP_LOGW(TAG, "GET /api/ambient: missing level field: %s", resp.buf);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    *level_out = level->valueint;
    ESP_LOGI(TAG, "GET /api/ambient OK: level=%d", *level_out);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t base_bridge_post_arm_gesture(const char *gesture)
{
    if (!gesture || strlen(gesture) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char body[96];
    snprintf(body, sizeof(body), "{\"gesture\":\"%s\"}", gesture);
    return bridge_post_json("/api/arm/gesture", body);
}

esp_err_t base_bridge_post_detect_phone(const char *source)
{
    if (!source || strlen(source) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char body[96];
    snprintf(body, sizeof(body), "{\"source\":\"%s\"}", source);
    return bridge_post_json("/api/detect/phone", body);
}
