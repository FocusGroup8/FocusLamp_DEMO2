/*
 * tts_http_api.c - HTTP endpoints for remote control from the head/base board
 *
 * Provides:
 *   POST /api/tts/speak  - TTS text injection (played through this speaker)
 *   POST /api/chat/end   - end the current xiaozhi voice conversation
 *   POST /api/detect/phone - enqueue a phone/computer reminder locally
 *   GET  /api/ambient   - bounded local ADC sampling (estimated lux)
 *   POST /api/arm/gesture - serial local executor; accepted != completed
 *   POST /api/arm/stop - bounded soft stop, preserves torque
 *
 * The dowm (FocusLamp base) board sends TTS text; the head board can also
 * end an active voice session (long-press on the head light).
 *
 * Request:  POST /api/tts/speak
 * Body:     {"text": "播报的文字内容", "priority": 1}
 * Response: {"ok": true}
 *
 * Request:  POST /api/chat/end
 * Body:     {} (optional, ignored)
 * Response: {"ok": true}
 */

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "websocket_manager.h"
#include "xiaozhi_manager.h"
#include "motion_controller.h"
#include "light_sensor_driver.h"
#include "device_state.h"
#include <math.h>

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
 * HTTP handler: POST /api/chat/end
 *
 * Ends the current xiaozhi voice conversation (closes the audio channel,
 * aborting any ongoing TTS). The wake word engine stays active, so a new
 * conversation can still be started by voice afterwards.
 *-------------------------------------------------------------*/
static esp_err_t chat_end_handler(httpd_req_t *req)
{
    /* Drain the (ignored) request body, if any */
    if (req->content_len > 0) {
        char *body = malloc(req->content_len + 1);
        if (body) {
            int received = 0;
            while (received < req->content_len) {
                int ret = httpd_req_recv(req, body + received, req->content_len - received);
                if (ret <= 0) {
                    if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                        continue;
                    }
                    break;
                }
                received += ret;
            }
            free(body);
        }
    }

    ESP_LOGI(TAG, "Chat end requested");

    esp_err_t ret = xiaozhi_manager_close_audio_channel();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "xiaozhi_manager_close_audio_channel failed: %s", esp_err_to_name(ret));
    }

    httpd_resp_set_type(req, "application/json");
    const char *resp = "{\"ok\":true}";
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

static const httpd_uri_t chat_end_uri = {
    .uri      = "/api/chat/end",
    .method   = HTTP_POST,
    .handler  = chat_end_handler,
    .user_ctx = NULL,
};

/* L5.0: only successful local enqueue is reported as HTTP 200. */
static esp_err_t detect_phone_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    if (req->content_len == 0 || req->content_len > INT_MAX - 1) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid body length\"}");
        return ESP_FAIL;
    }

    int total = (int)req->content_len;
    char *body = malloc((size_t)total + 1);
    if (!body) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no memory\"}");
        return ESP_ERR_NO_MEM;
    }

    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, body + received, total - received);
        if (ret <= 0) {
            free(body);
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                httpd_resp_set_status(req, "408 Request Timeout");
            } else {
                httpd_resp_set_status(req, "400 Bad Request");
            }
            httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"read failed\"}");
            return ESP_FAIL; /* Close the session if the body was incomplete. */
        }
        received += ret;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid JSON\"}");
        return ESP_FAIL;
    }

    cJSON *source = cJSON_GetObjectItem(root, "source");
    if (!cJSON_IsString(source) || !source->valuestring) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"missing 'source' field\"}");
        return ESP_FAIL;
    }

    const char *text;
    if (strcmp(source->valuestring, "phone") == 0) {
        text = "提醒玩手机";
    } else if (strcmp(source->valuestring, "computer") == 0) {
        text = "提醒玩电脑";
    } else {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"unknown source\"}");
        return ESP_FAIL;
    }
    cJSON_Delete(root);

    esp_err_t ret = xiaozhi_manager_speak(text, 1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Detect reminder enqueue failed: %s", esp_err_to_name(ret));
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"tts enqueue failed\"}");
    }

    ESP_LOGI(TAG, "Detect reminder queued: %s", text);
    httpd_resp_set_status(req, "200 OK");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static const httpd_uri_t detect_phone_uri = {
    .uri = "/api/detect/phone",
    .method = HTTP_POST,
    .handler = detect_phone_handler,
    .user_ctx = NULL,
};

static esp_err_t ambient_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req,"application/json");
    if (!light_sensor_driver_is_initialized()) {
        httpd_resp_set_status(req,"503 Service Unavailable");
        return httpd_resp_sendstr(req,"{\"ok\":false,\"error\":\"ambient sensor not ready\"}");
    }
    float lux;
    esp_err_t ret=light_sensor_driver_read_averaged(&lux,8);
    if (ret!=ESP_OK || !isfinite(lux) || lux<0) {
        httpd_resp_set_status(req,"500 Internal Server Error");
        return httpd_resp_sendstr(req,"{\"ok\":false,\"error\":\"ADC sample failed\"}");
    }
    uint8_t level=lux>1000 ? 4 : lux>500 ? 3 : lux>100 ? 2 : lux>20 ? 1 : 0;
    ret=device_state_set_ambient_light(level,lux);
    if (ret!=ESP_OK) {
        httpd_resp_set_status(req,"500 Internal Server Error");
        return httpd_resp_sendstr(req,"{\"ok\":false,\"error\":\"ambient state update failed\"}");
    }
    char response[96];
    snprintf(response,sizeof(response),"{\"ok\":true,\"level\":%u,\"lux\":%.2f}",(unsigned)level,(double)lux);
    ESP_LOGI(TAG,"Ambient sampled: estimated lux=%.2f level=%u",(double)lux,(unsigned)level);
    return httpd_resp_sendstr(req,response);
}

static const httpd_uri_t ambient_get_uri = {
    .uri = "/api/ambient",
    .method = HTTP_GET,
    .handler = ambient_get_handler,
    .user_ctx = NULL,
};

/* These two arm endpoints use a small bounded body; old TTS handlers are unchanged. */
static esp_err_t arm_read_body(httpd_req_t *req,char body[129],bool required)
{
    httpd_resp_set_type(req,"application/json");
    if (req->content_len>128 || (required && req->content_len==0)) {
        httpd_resp_set_status(req,req->content_len>128 ? "413 Payload Too Large" : "400 Bad Request");
        httpd_resp_set_hdr(req,"Connection","close");
        httpd_resp_sendstr(req,"{\"ok\":false,\"error\":\"invalid body length\"}");
        return ESP_FAIL;
    }
    size_t done=0;
    while (done<req->content_len) {
        int n=httpd_req_recv(req,body+done,req->content_len-done);
        if (n<=0) {
            httpd_resp_set_status(req,n==HTTPD_SOCK_ERR_TIMEOUT ? "408 Request Timeout" : "400 Bad Request");
            httpd_resp_set_hdr(req,"Connection","close");
            httpd_resp_sendstr(req,"{\"ok\":false,\"error\":\"incomplete body\"}");
            return ESP_FAIL;
        }
        done+=(size_t)n;
    }
    body[done]='\0';
    if (memchr(body,'\0',done)) {
        httpd_resp_set_status(req,"400 Bad Request");
        httpd_resp_sendstr(req,"{\"ok\":false,\"error\":\"invalid body\"}");
        return ESP_FAIL;
    }
    return ESP_OK;
}
static esp_err_t arm_response(httpd_req_t *req,esp_err_t ret,const char *success)
{
    if (ret==ESP_OK) return httpd_resp_sendstr(req,success);
    motion_controller_status_t state;
    bool ready=motion_controller_get_status(&state)==ESP_OK;
    httpd_resp_set_status(req,!ready ? "503 Service Unavailable" :
        ret==ESP_ERR_INVALID_ARG ? "400 Bad Request" :
        ret==ESP_ERR_TIMEOUT || ret==ESP_ERR_INVALID_STATE ? "409 Conflict" : "500 Internal Server Error");
    char response[96];
    snprintf(response,sizeof(response),"{\"ok\":false,\"error\":\"%s\"}",esp_err_to_name(ret));
    return httpd_resp_sendstr(req,response);
}

static esp_err_t arm_gesture_handler(httpd_req_t *req)
{
    char body[129];
    if (arm_read_body(req,body,true)!=ESP_OK) return ESP_FAIL;
    cJSON *root=cJSON_ParseWithOpts(body,NULL,true);
    const cJSON *gesture=cJSON_GetObjectItemCaseSensitive(root,"gesture");
    if (!cJSON_IsObject(root) || !cJSON_IsString(gesture) ||
        (strcmp(gesture->valuestring,"thumb_up") && strcmp(gesture->valuestring,"thumb_down"))) {
        cJSON_Delete(root);
        httpd_resp_set_status(req,"400 Bad Request");
        return httpd_resp_sendstr(req,"{\"ok\":false,\"error\":\"unknown gesture\"}");
    }
    esp_err_t ret=motion_gesture_async(gesture->valuestring);
    ESP_LOGI(TAG,"Arm gesture %s: %s (accepted != completed)",gesture->valuestring,esp_err_to_name(ret));
    cJSON_Delete(root);
    return arm_response(req,ret,"{\"ok\":true,\"status\":\"accepted\"}");
}

static const httpd_uri_t arm_gesture_uri = {
    .uri = "/api/arm/gesture",
    .method = HTTP_POST,
    .handler = arm_gesture_handler,
    .user_ctx = NULL,
};

/*---------------------------------------------------------------
 * Init
 *-------------------------------------------------------------*/

static esp_err_t arm_stop_handler(httpd_req_t *req)
{
    char body[129];
    if (arm_read_body(req,body,false)!=ESP_OK) return ESP_FAIL;
    return arm_response(req,motion_action_stop(),
        "{\"ok\":true,\"status\":\"soft_stopped\"}");
}
static const httpd_uri_t arm_stop_uri={
    .uri="/api/arm/stop", .method=HTTP_POST, .handler=arm_stop_handler,
};

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

    ret = ws_manager_server_register_uri(&chat_end_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /api/chat/end: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = ws_manager_server_register_uri(&detect_phone_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /api/detect/phone: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = ws_manager_server_register_uri(&ambient_get_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /api/ambient: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = ws_manager_server_register_uri(&arm_gesture_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /api/arm/gesture: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = ws_manager_server_register_uri(&arm_stop_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /api/arm/stop: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "TTS API endpoints registered: POST /api/tts/speak, POST /api/chat/end");
    ESP_LOGI(TAG, "Local endpoints registered: POST /api/detect/phone, GET /api/ambient, POST /api/arm/gesture, POST /api/arm/stop");
    return ESP_OK;
}
