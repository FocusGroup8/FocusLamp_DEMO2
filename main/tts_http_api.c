/*
 * tts_http_api.c - HTTP endpoint for TTS text injection
 *
 * Provides POST /api/tts/speak endpoint so the dowm (FocusLamp base)
 * board can send TTS text to be played through this device's speaker.
 *
 * Request:  POST /api/tts/speak
 * Body:     {"text": "播报的文字内容", "priority": 1}
 * Response: {"ok": true}
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "websocket_manager.h"
#include "xiaozhi_manager.h"

static const char *TAG = "TTS_API";

/* Max TTS text length */
#define TTS_TEXT_MAX_LEN 256

/*---------------------------------------------------------------
 * HTTP handler: POST /api/tts/speak
 *-------------------------------------------------------------*/
static esp_err_t tts_speak_handler(httpd_req_t *req)
{
    char *body = malloc(req->content_len + 1);
    if (!body) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no memory\"}");
        return ESP_ERR_NO_MEM;
    }

    int received = 0;
    int total = req->content_len;
    if (total < 0) total = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, body + received, total - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            free(body);
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"read failed\"}");
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';

    /* Parse JSON */
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        free(body);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid JSON\"}");
        return ESP_FAIL;
    }

    cJSON *text_item = cJSON_GetObjectItem(root, "text");
    if (!cJSON_IsString(text_item) || !text_item->valuestring) {
        cJSON_Delete(root);
        free(body);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"missing 'text' field\"}");
        return ESP_FAIL;
    }

    const char *text = text_item->valuestring;

    /* Optional priority (default 1) */
    int priority = 1;
    cJSON *prio_item = cJSON_GetObjectItem(root, "priority");
    if (cJSON_IsNumber(prio_item)) {
        priority = prio_item->valueint;
    }

    ESP_LOGI(TAG, "TTS speak: \"%s\" (priority=%d)", text, priority);

    /* Call xiaozhi_manager to play TTS */
    esp_err_t ret = xiaozhi_manager_speak(text, priority);

    cJSON_Delete(root);
    free(body);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "xiaozhi_manager_speak failed: %s", esp_err_to_name(ret));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"xiaozhi speak failed\"}");
        return ESP_OK; /* Don't fail the HTTP request */
    }

    httpd_resp_set_type(req, "application/json");
    const char *resp = "{\"ok\":true}";
    httpd_resp_send(req, resp, strlen(resp));

    return ESP_OK;
}

static const httpd_uri_t tts_speak_uri = {
    .uri      = "/api/tts/speak",
    .method   = HTTP_POST,
    .handler  = tts_speak_handler,
    .user_ctx = NULL,
};

/*---------------------------------------------------------------
 * Init
 *-------------------------------------------------------------*/
esp_err_t tts_http_api_init(void)
{
    if (!ws_manager_server_is_running()) {
        ESP_LOGE(TAG, "WebSocket server not running, cannot register URI");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ws_manager_server_register_uri(&tts_speak_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /api/tts/speak: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "TTS API endpoint registered: POST /api/tts/speak");
    return ESP_OK;
}
