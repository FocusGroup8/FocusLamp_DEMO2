/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "network_manager.h"

#include "base_bridge.h"
#include "cJSON.h"
#include "camera_stream.h"
#include "connection_manager.h"
#include "display_system.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "heartbeat_service.h"
#if CONFIG_EXAMPLE_ENABLE_LED
#include "led_controller.h"
#endif
#if CONFIG_EXAMPLE_DEMO_EXPRESSIVE_EYES
#include "expressive_eyes_display.h"
#endif
#include "mcp_tools.h"
#include "message_queue.h"
#include "system_manager.h"
#include "websocket_manager.h"
#include "wifi_manager.h"

#include <string.h>

static const char *TAG = "net_mgr";

#define MCP_RESPONSE_BUF_SIZE 4096
#define WIFI_CONNECT_TIMEOUT_MS 30000

static bool s_initialized                     = false;
static SemaphoreHandle_t s_wifi_connected_sem = NULL;
static char s_ip_string[16]                   = {0};

/* Latest algorithm result state (written by algorithm.result tool callback,
 * read by network_manager_get_algo_result_state()). */
static algo_result_state_t s_algo_state = {0};
static SemaphoreHandle_t s_algo_mutex   = NULL;

/*---------------------------------------------------------------
 * VLM detection forward (A1) and gesture forward (A2) throttle state
 *-------------------------------------------------------------*/
/* A1: forward phone/computer detection once per source every >=60s;
 * a source change forwards immediately and resets the timer. */
static char s_last_vlm_source[16]    = {0};
static int64_t s_last_vlm_forward_us = 0;
#define VLM_FORWARD_THROTTLE_US (60LL * 1000LL * 1000LL)

/* A2: forward thumb up/down only on gesture change, throttled to >=1s. */
static char s_last_gesture[32]    = {0};
static int64_t s_last_gesture_us  = 0;
#define GESTURE_FORWARD_THROTTLE_US (1LL * 1000LL * 1000LL)

/*---------------------------------------------------------------
 * Internal: WiFi event callback
 *-------------------------------------------------------------*/
static void wifi_event_handler(wifi_manager_event_t event, void *data)
{
    switch (event) {
    case WIFI_MANAGER_EVENT_GOT_IP:
        ESP_LOGI(TAG, "WiFi got IP");
        if (s_wifi_connected_sem) {
            xSemaphoreGive(s_wifi_connected_sem);
        }
        break;
    case WIFI_MANAGER_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WiFi connected");
        break;
    case WIFI_MANAGER_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WiFi disconnected");
        break;
    default:
        break;
    }
}

/*---------------------------------------------------------------
 * Internal: WebSocket data callback - dispatch /mcp messages
 *-------------------------------------------------------------*/
static void ws_data_handler(ws_manager_event_t event, void *data)
{
    if (event != WS_MANAGER_EVENT_DATA || !data) {
        return;
    }

    ws_manager_data_t *msg = (ws_manager_data_t *)data;

    /* Only handle text messages on /mcp or /algo path */
    if (msg->type != WS_DATA_TYPE_TEXT || !msg->uri) {
        return;
    }

    if (strcmp(msg->uri, "/mcp") != 0 && strcmp(msg->uri, "/algo") != 0) {
        return;
    }

    /* Ensure null-terminated (copy to local buffer) */
    char request_buf[MCP_RESPONSE_BUF_SIZE];
    int copy_len = msg->data_len;
    if (copy_len >= (int)sizeof(request_buf)) {
        copy_len = sizeof(request_buf) - 1;
    }
    memcpy(request_buf, msg->data, copy_len);
    request_buf[copy_len] = '\0';

    /* Handle MCP message */
    char response_buf[MCP_RESPONSE_BUF_SIZE];
    esp_err_t ret = mcp_tools_handle_message(request_buf, response_buf, sizeof(response_buf));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "MCP handle failed: %s", esp_err_to_name(ret));
        return;
    }

    /* Send response back to client via message queue for reliable delivery */
    if (msg->client_fd >= 0 && response_buf[0] != '\0') {
        ret = message_queue_enqueue(msg->client_fd, response_buf, strlen(response_buf), MSG_QUEUE_TYPE_TEXT);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "MCP response enqueue failed: %s", esp_err_to_name(ret));
        }
    }
}

/*---------------------------------------------------------------
 * MCP Tool Callbacks: Camera control
 * Real implementations calling camera_stream_* APIs.
 *-------------------------------------------------------------*/
static esp_err_t mcp_cb_camera_start(const void *args_json, char *response_buf, int response_buf_size)
{
    (void)args_json;
    esp_err_t ret = camera_stream_start();
    snprintf(response_buf, response_buf_size, "{\"ok\":%s}", ret == ESP_OK ? "true" : "false");
    return ret;
}

static esp_err_t mcp_cb_camera_stop(const void *args_json, char *response_buf, int response_buf_size)
{
    (void)args_json;
    esp_err_t ret = camera_stream_stop();
    snprintf(response_buf, response_buf_size, "{\"ok\":%s}", ret == ESP_OK ? "true" : "false");
    return ret;
}

static esp_err_t mcp_cb_camera_set_quality(const void *args_json, char *response_buf, int response_buf_size)
{
    const cJSON *root = (const cJSON *)args_json;
    int quality       = 15;
    if (root) {
        const cJSON *q = cJSON_GetObjectItem(root, "quality");
        if (cJSON_IsNumber(q)) {
            quality = q->valueint;
        }
    }
    esp_err_t ret = camera_stream_set_quality(quality);
    snprintf(response_buf, response_buf_size, "{\"ok\":%s,\"quality\":%d}", ret == ESP_OK ? "true" : "false", quality);
    return ret;
}

static esp_err_t mcp_cb_camera_set_fps(const void *args_json, char *response_buf, int response_buf_size)
{
    const cJSON *root = (const cJSON *)args_json;
    int fps           = 15;
    if (root) {
        const cJSON *f = cJSON_GetObjectItem(root, "fps");
        if (cJSON_IsNumber(f)) {
            fps = f->valueint;
        }
    }
    esp_err_t ret = camera_stream_set_fps(fps);
    snprintf(response_buf, response_buf_size, "{\"ok\":%s,\"fps\":%d}", ret == ESP_OK ? "true" : "false", fps);
    return ret;
}

/*---------------------------------------------------------------
 * MCP Tool Callbacks: Display control
 * TODO: Implement with display_system_* APIs when available.
 *-------------------------------------------------------------*/
static esp_err_t mcp_cb_display_on(const void *args_json, char *response_buf, int response_buf_size)
{
    (void)args_json;
    esp_err_t ret = display_system_show_ui();
    ESP_LOGI(TAG, "MCP: Display ON (show UI) -> %s", esp_err_to_name(ret));
    snprintf(response_buf, response_buf_size, "{\"ok\":%s}", ret == ESP_OK ? "true" : "false");
    return ESP_OK;
}

static esp_err_t mcp_cb_display_off(const void *args_json, char *response_buf, int response_buf_size)
{
    (void)args_json;
    esp_err_t ret = display_system_show_black_screen();
    ESP_LOGI(TAG, "MCP: Display OFF (black screen) -> %s", esp_err_to_name(ret));
    snprintf(response_buf, response_buf_size, "{\"ok\":%s}", ret == ESP_OK ? "true" : "false");
    return ESP_OK;
}

static esp_err_t mcp_cb_display_set_brightness(const void *args_json, char *response_buf, int response_buf_size)
{
    const cJSON *root = (const cJSON *)args_json;
    int level         = 80;
    if (root) {
        const cJSON *l = cJSON_GetObjectItem(root, "level");
        if (cJSON_IsNumber(l)) {
            level = l->valueint;
        }
    }
    /* Backlight is fixed (not PWM). Use visual on/off: level > 0 = show UI, level == 0 = black screen */
    esp_err_t err = (level > 0) ? display_system_show_ui() : display_system_show_black_screen();
    ESP_LOGI(TAG, "MCP: Display brightness %d -> %s (%s)", level, level > 0 ? "show UI" : "black screen",
             esp_err_to_name(err));
    snprintf(response_buf, response_buf_size, "{\"ok\":%s,\"level\":%d}", err == ESP_OK ? "true" : "false", level);
    return ESP_OK;
}

static esp_err_t mcp_cb_display_show_camera(const void *args_json, char *response_buf, int response_buf_size)
{
    (void)args_json;
    /* TODO: call camera_preview_start/stop when available */
    snprintf(response_buf, response_buf_size, "{\"ok\":true}");
    return ESP_OK;
}

/*---------------------------------------------------------------
 * REST API handlers for wifi_test HTTP bridge
 *-------------------------------------------------------------*/
static esp_err_t rest_api_camera_start_handler(httpd_req_t *req)
{
    esp_err_t ret    = camera_stream_start();
    const char *resp = ret == ESP_OK ? "{\"code\":0,\"message\":\"success\",\"data\":{}}"
                                     : "{\"code\":1,\"message\":\"camera start failed\",\"data\":{}}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t rest_api_camera_stop_handler(httpd_req_t *req)
{
    esp_err_t ret    = camera_stream_stop();
    const char *resp = ret == ESP_OK ? "{\"code\":0,\"message\":\"success\",\"data\":{}}"
                                     : "{\"code\":1,\"message\":\"camera stop failed\",\"data\":{}}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t rest_api_camera_quality_handler(httpd_req_t *req)
{
    char buf[32] = {0};
    int ret      = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(buf);
    int quality = 15;
    if (root) {
        cJSON *q = cJSON_GetObjectItem(root, "quality");
        if (cJSON_IsNumber(q)) {
            quality = q->valueint;
        }
        cJSON_Delete(root);
    }

    esp_err_t err = camera_stream_set_quality(quality);
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"code\":%d,\"message\":\"%s\",\"data\":{\"quality\":%d}}", err == ESP_OK ? 0 : 1,
             err == ESP_OK ? "success" : "set quality failed", quality);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t rest_api_camera_fps_handler(httpd_req_t *req)
{
    char buf[32] = {0};
    int ret      = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(buf);
    int fps     = 15;
    if (root) {
        cJSON *f = cJSON_GetObjectItem(root, "fps");
        if (cJSON_IsNumber(f)) {
            fps = f->valueint;
        }
        cJSON_Delete(root);
    }

    esp_err_t err = camera_stream_set_fps(fps);
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"code\":%d,\"message\":\"%s\",\"data\":{\"fps\":%d}}", err == ESP_OK ? 0 : 1,
             err == ESP_OK ? "success" : "set fps failed", fps);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t rest_api_display_on_handler(httpd_req_t *req)
{
    esp_err_t ret = display_system_show_ui();
    ESP_LOGI(TAG, "REST: Display ON (show UI) -> %s", esp_err_to_name(ret));
    const char *resp = ret == ESP_OK ? "{\"code\":0,\"message\":\"success\",\"data\":{}}"
                                     : "{\"code\":1,\"message\":\"display on failed\",\"data\":{}}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t rest_api_display_off_handler(httpd_req_t *req)
{
    esp_err_t ret = display_system_show_black_screen();
    ESP_LOGI(TAG, "REST: Display OFF (black screen) -> %s", esp_err_to_name(ret));
    const char *resp = ret == ESP_OK ? "{\"code\":0,\"message\":\"success\",\"data\":{}}"
                                     : "{\"code\":1,\"message\":\"display off failed\",\"data\":{}}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t rest_api_display_brightness_handler(httpd_req_t *req)
{
    char buf[32] = {0};
    int ret      = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(buf);
    int level   = 80;
    if (root) {
        cJSON *l = cJSON_GetObjectItem(root, "level");
        if (cJSON_IsNumber(l)) {
            level = l->valueint;
        }
        cJSON_Delete(root);
    }

    /* Backlight is fixed (not PWM). Use visual on/off: level > 0 = show UI, level == 0 = black screen */
    esp_err_t err = (level > 0) ? display_system_show_ui() : display_system_show_black_screen();
    ESP_LOGI(TAG, "REST: Display brightness %d -> %s (%s)", level, level > 0 ? "show UI" : "black screen",
             esp_err_to_name(err));
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"code\":%d,\"message\":\"%s\",\"data\":{\"level\":%d}}", err == ESP_OK ? 0 : 1,
             err == ESP_OK ? "success" : "not supported", level);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t rest_api_display_camera_preview_handler(httpd_req_t *req)
{
    /* TODO: call camera_preview_start/stop */
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"code\":0,\"message\":\"success\",\"data\":{}}");
    return ESP_OK;
}

static esp_err_t rest_api_status_handler(httpd_req_t *req)
{
    char resp[160];
    snprintf(resp, sizeof(resp),
             "{\"code\":0,\"message\":\"success\",\"data\":{\"camera_streaming\":%s,\"quality\":%d,\"fps\":%d}}",
             camera_stream_is_running() ? "true" : "false", camera_stream_get_quality(), camera_stream_get_fps());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/*---------------------------------------------------------------
 * REST API handlers for LED control
 *-------------------------------------------------------------*/
#if CONFIG_EXAMPLE_ENABLE_LED

static esp_err_t rest_api_led_on_handler(httpd_req_t *req)
{
    /* Parse optional JSON body for brightness and color_temp */
    int brightness = 100;
    int color_temp = -1; /* -1 means not specified, keep current */

    if (req->content_len > 0 && req->content_len < 128) {
        char buf[128] = {0};
        int ret       = httpd_req_recv(req, buf, sizeof(buf) - 1);
        if (ret > 0) {
            cJSON *root = cJSON_Parse(buf);
            if (root) {
                cJSON *b = cJSON_GetObjectItem(root, "brightness");
                if (cJSON_IsNumber(b)) {
                    brightness = b->valueint;
                }
                cJSON *ct = cJSON_GetObjectItem(root, "color_temp");
                if (cJSON_IsNumber(ct)) {
                    color_temp = ct->valueint;
                }
                cJSON_Delete(root);
            }
        }
    }

    /* Clamp brightness to valid range */
    if (brightness < 0)
        brightness = 0;
    if (brightness > 100)
        brightness = 100;

    esp_err_t err;
    if (color_temp >= 0) {
        if (color_temp > 100)
            color_temp = 100;
        err = led_set_color_temp((uint8_t)color_temp);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "LED set color_temp failed: %s", esp_err_to_name(err));
        }
    }
    err = led_set_brightness_with_cct((uint8_t)brightness);

    ESP_LOGI(TAG, "REST: LED ON brightness=%d, color_temp=%d -> %s", brightness, color_temp, esp_err_to_name(err));

    char resp[96];
    snprintf(resp, sizeof(resp), "{\"code\":%d,\"message\":\"%s\",\"data\":{\"brightness\":%d,\"color_temp\":%d}}",
             err == ESP_OK ? 0 : 1, err == ESP_OK ? "success" : "led on failed", brightness,
             color_temp >= 0 ? color_temp : -1);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t rest_api_led_off_handler(httpd_req_t *req)
{
    esp_err_t err = led_off();
    ESP_LOGI(TAG, "REST: LED OFF -> %s", esp_err_to_name(err));

    const char *resp = err == ESP_OK ? "{\"code\":0,\"message\":\"success\",\"data\":{}}"
                                     : "{\"code\":1,\"message\":\"led off failed\",\"data\":{}}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t rest_api_led_brightness_handler(httpd_req_t *req)
{
    char buf[64] = {0};
    int ret      = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON *root    = cJSON_Parse(buf);
    int brightness = 100;
    if (root) {
        cJSON *b = cJSON_GetObjectItem(root, "brightness");
        if (cJSON_IsNumber(b)) {
            brightness = b->valueint;
        }
        cJSON_Delete(root);
    }

    if (brightness < 0)
        brightness = 0;
    if (brightness > 100)
        brightness = 100;

    esp_err_t err = led_set_brightness_with_cct((uint8_t)brightness);
    ESP_LOGI(TAG, "REST: LED brightness %d -> %s", brightness, esp_err_to_name(err));

    char resp[96];
    snprintf(resp, sizeof(resp), "{\"code\":%d,\"message\":\"%s\",\"data\":{\"brightness\":%d}}", err == ESP_OK ? 0 : 1,
             err == ESP_OK ? "success" : "set brightness failed", brightness);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t rest_api_led_color_temp_handler(httpd_req_t *req)
{
    char buf[64] = {0};
    int ret      = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON *root    = cJSON_Parse(buf);
    int color_temp = 50;
    if (root) {
        cJSON *ct = cJSON_GetObjectItem(root, "color_temp");
        if (cJSON_IsNumber(ct)) {
            color_temp = ct->valueint;
        }
        cJSON_Delete(root);
    }

    if (color_temp < 0)
        color_temp = 0;
    if (color_temp > 100)
        color_temp = 100;

    esp_err_t err = led_set_color_temp((uint8_t)color_temp);
    ESP_LOGI(TAG, "REST: LED color_temp %d -> %s", color_temp, esp_err_to_name(err));

    char resp[96];
    snprintf(resp, sizeof(resp), "{\"code\":%d,\"message\":\"%s\",\"data\":{\"color_temp\":%d}}", err == ESP_OK ? 0 : 1,
             err == ESP_OK ? "success" : "set color_temp failed", color_temp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t rest_api_led_status_handler(httpd_req_t *req)
{
    bool initialized = led_is_initialized();
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"code\":0,\"message\":\"success\",\"data\":{\"initialized\":%s}}",
             initialized ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

#else /* CONFIG_EXAMPLE_ENABLE_LED == 0 */

/* Stub handlers when LED is disabled */
static esp_err_t rest_api_led_on_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"code\":1,\"message\":\"led not supported\",\"data\":{}}");
    return ESP_ERR_NOT_SUPPORTED;
}
static esp_err_t rest_api_led_off_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"code\":1,\"message\":\"led not supported\",\"data\":{}}");
    return ESP_ERR_NOT_SUPPORTED;
}
static esp_err_t rest_api_led_brightness_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"code\":1,\"message\":\"led not supported\",\"data\":{}}");
    return ESP_ERR_NOT_SUPPORTED;
}
static esp_err_t rest_api_led_color_temp_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"code\":1,\"message\":\"led not supported\",\"data\":{}}");
    return ESP_ERR_NOT_SUPPORTED;
}
static esp_err_t rest_api_led_status_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"code\":0,\"message\":\"success\",\"data\":{\"initialized\":false}}");
    return ESP_OK;
}

#endif /* CONFIG_EXAMPLE_ENABLE_LED */

/*---------------------------------------------------------------
 * MCP Tool Callbacks: LED control
 *-------------------------------------------------------------*/
#if CONFIG_EXAMPLE_ENABLE_LED

static esp_err_t mcp_cb_led_on(const void *args_json, char *response_buf, int response_buf_size)
{
    int brightness = 100;
    int color_temp = -1;

    const cJSON *root = (const cJSON *)args_json;
    if (root) {
        const cJSON *b = cJSON_GetObjectItem(root, "brightness");
        if (cJSON_IsNumber(b)) {
            brightness = b->valueint;
        }
        const cJSON *ct = cJSON_GetObjectItem(root, "color_temp");
        if (cJSON_IsNumber(ct)) {
            color_temp = ct->valueint;
        }
    }

    if (brightness < 0)
        brightness = 0;
    if (brightness > 100)
        brightness = 100;

    esp_err_t err;
    if (color_temp >= 0) {
        if (color_temp > 100)
            color_temp = 100;
        led_set_color_temp((uint8_t)color_temp);
    }
    err = led_set_brightness_with_cct((uint8_t)brightness);

    ESP_LOGI(TAG, "MCP: LED ON brightness=%d, color_temp=%d -> %s", brightness, color_temp, esp_err_to_name(err));
    snprintf(response_buf, response_buf_size, "{\"ok\":%s,\"brightness\":%d,\"color_temp\":%d}",
             err == ESP_OK ? "true" : "false", brightness, color_temp >= 0 ? color_temp : -1);
    return err;
}

static esp_err_t mcp_cb_led_off(const void *args_json, char *response_buf, int response_buf_size)
{
    (void)args_json;
    esp_err_t err = led_off();
    ESP_LOGI(TAG, "MCP: LED OFF -> %s", esp_err_to_name(err));
    snprintf(response_buf, response_buf_size, "{\"ok\":%s}", err == ESP_OK ? "true" : "false");
    return err;
}

static esp_err_t mcp_cb_led_set_brightness(const void *args_json, char *response_buf, int response_buf_size)
{
    int brightness    = 100;
    const cJSON *root = (const cJSON *)args_json;
    if (root) {
        const cJSON *b = cJSON_GetObjectItem(root, "brightness");
        if (cJSON_IsNumber(b)) {
            brightness = b->valueint;
        }
    }

    if (brightness < 0)
        brightness = 0;
    if (brightness > 100)
        brightness = 100;

    esp_err_t err = led_set_brightness_with_cct((uint8_t)brightness);
    ESP_LOGI(TAG, "MCP: LED set_brightness %d -> %s", brightness, esp_err_to_name(err));
    snprintf(response_buf, response_buf_size, "{\"ok\":%s,\"brightness\":%d}", err == ESP_OK ? "true" : "false",
             brightness);
    return err;
}

static esp_err_t mcp_cb_led_set_color_temp(const void *args_json, char *response_buf, int response_buf_size)
{
    int color_temp    = 50;
    const cJSON *root = (const cJSON *)args_json;
    if (root) {
        const cJSON *ct = cJSON_GetObjectItem(root, "color_temp");
        if (cJSON_IsNumber(ct)) {
            color_temp = ct->valueint;
        }
    }

    if (color_temp < 0)
        color_temp = 0;
    if (color_temp > 100)
        color_temp = 100;

    esp_err_t err = led_set_color_temp((uint8_t)color_temp);
    ESP_LOGI(TAG, "MCP: LED set_color_temp %d -> %s", color_temp, esp_err_to_name(err));
    snprintf(response_buf, response_buf_size, "{\"ok\":%s,\"color_temp\":%d}", err == ESP_OK ? "true" : "false",
             color_temp);
    return err;
}

#else /* CONFIG_EXAMPLE_ENABLE_LED == 0 */

static esp_err_t mcp_cb_led_on(const void *args_json, char *response_buf, int response_buf_size)
{
    (void)args_json;
    snprintf(response_buf, response_buf_size, "{\"ok\":false}");
    return ESP_ERR_NOT_SUPPORTED;
}
static esp_err_t mcp_cb_led_off(const void *args_json, char *response_buf, int response_buf_size)
{
    (void)args_json;
    snprintf(response_buf, response_buf_size, "{\"ok\":false}");
    return ESP_ERR_NOT_SUPPORTED;
}
static esp_err_t mcp_cb_led_set_brightness(const void *args_json, char *response_buf, int response_buf_size)
{
    (void)args_json;
    snprintf(response_buf, response_buf_size, "{\"ok\":false}");
    return ESP_ERR_NOT_SUPPORTED;
}
static esp_err_t mcp_cb_led_set_color_temp(const void *args_json, char *response_buf, int response_buf_size)
{
    (void)args_json;
    snprintf(response_buf, response_buf_size, "{\"ok\":false}");
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_EXAMPLE_ENABLE_LED */

/*---------------------------------------------------------------
 * MCP Tool Callbacks: Expressive Eyes control
 *-------------------------------------------------------------*/
#if CONFIG_EXAMPLE_DEMO_EXPRESSIVE_EYES

static const char *s_expression_names[] = {"neutral", "happy", "sad",       "angry",     "surprised",
                                           "sleepy",  "bored", "wink_left", "wink_right"};

static int expression_from_string(const char *str)
{
    for (int i = 0; i < (int)(sizeof(s_expression_names) / sizeof(s_expression_names[0])); i++) {
        if (strcmp(str, s_expression_names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static esp_err_t mcp_cb_eyes_set_expression(const void *args_json, char *response_buf, int response_buf_size)
{
    const cJSON *root = (const cJSON *)args_json;
    if (!root) {
        snprintf(response_buf, response_buf_size, "{\"ok\":false,\"error\":\"missing arguments\"}");
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *expr_item = cJSON_GetObjectItem(root, "expression");
    if (!cJSON_IsString(expr_item) || !expr_item->valuestring) {
        snprintf(response_buf, response_buf_size, "{\"ok\":false,\"error\":\"missing expression\"}");
        return ESP_ERR_INVALID_ARG;
    }

    int expr = expression_from_string(expr_item->valuestring);
    if (expr < 0) {
        snprintf(response_buf, response_buf_size, "{\"ok\":false,\"error\":\"unknown expression: %s\"}",
                 expr_item->valuestring);
        return ESP_ERR_INVALID_ARG;
    }

    expressive_eyes_set_expression((expressive_eyes_expression_t)expr);
    ESP_LOGI(TAG, "MCP: Eyes set_expression -> %s", expr_item->valuestring);
    snprintf(response_buf, response_buf_size, "{\"ok\":true,\"expression\":\"%s\"}", expr_item->valuestring);
    return ESP_OK;
}

static esp_err_t mcp_cb_eyes_look_at(const void *args_json, char *response_buf, int response_buf_size)
{
    const cJSON *root = (const cJSON *)args_json;
    float x = 0.0f, y = 0.0f;

    if (root) {
        const cJSON *x_item = cJSON_GetObjectItem(root, "x");
        const cJSON *y_item = cJSON_GetObjectItem(root, "y");
        if (cJSON_IsNumber(x_item))
            x = (float)x_item->valuedouble;
        if (cJSON_IsNumber(y_item))
            y = (float)y_item->valuedouble;
    }

    /* Clamp to [-1, 1] */
    if (x < -1.0f)
        x = -1.0f;
    if (x > 1.0f)
        x = 1.0f;
    if (y < -1.0f)
        y = -1.0f;
    if (y > 1.0f)
        y = 1.0f;

    expressive_eyes_look_at(x, y);
    ESP_LOGI(TAG, "MCP: Eyes look_at -> (%.2f, %.2f)", x, y);
    snprintf(response_buf, response_buf_size, "{\"ok\":true,\"x\":%.2f,\"y\":%.2f}", x, y);
    return ESP_OK;
}

static esp_err_t mcp_cb_eyes_blink(const void *args_json, char *response_buf, int response_buf_size)
{
    (void)args_json;
    expressive_eyes_blink();
    ESP_LOGI(TAG, "MCP: Eyes blink");
    snprintf(response_buf, response_buf_size, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t mcp_cb_eyes_get_expression(const void *args_json, char *response_buf, int response_buf_size)
{
    (void)args_json;
    expressive_eyes_expression_t expr = expressive_eyes_get_expression();
    int idx                           = (int)expr;
    const char *name = (idx >= 0 && idx < (int)(sizeof(s_expression_names) / sizeof(s_expression_names[0])))
                           ? s_expression_names[idx]
                           : "unknown";
    snprintf(response_buf, response_buf_size, "{\"ok\":true,\"expression\":\"%s\"}", name);
    return ESP_OK;
}

#else /* CONFIG_EXAMPLE_DEMO_EXPRESSIVE_EYES == 0 */

static esp_err_t mcp_cb_eyes_set_expression(const void *args_json, char *response_buf, int response_buf_size)
{
    (void)args_json;
    snprintf(response_buf, response_buf_size, "{\"ok\":false}");
    return ESP_ERR_NOT_SUPPORTED;
}
static esp_err_t mcp_cb_eyes_look_at(const void *args_json, char *response_buf, int response_buf_size)
{
    (void)args_json;
    snprintf(response_buf, response_buf_size, "{\"ok\":false}");
    return ESP_ERR_NOT_SUPPORTED;
}
static esp_err_t mcp_cb_eyes_blink(const void *args_json, char *response_buf, int response_buf_size)
{
    (void)args_json;
    snprintf(response_buf, response_buf_size, "{\"ok\":false}");
    return ESP_ERR_NOT_SUPPORTED;
}
static esp_err_t mcp_cb_eyes_get_expression(const void *args_json, char *response_buf, int response_buf_size)
{
    (void)args_json;
    snprintf(response_buf, response_buf_size, "{\"ok\":false}");
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_EXAMPLE_DEMO_EXPRESSIVE_EYES */

/*---------------------------------------------------------------
 * MCP Tool Callback: Algorithm result ingestion (algorithm.result)
 * Parses detection results pushed by main-client on /algo endpoint and
 * stores them into s_algo_state under mutex. Execution logic is left as
 * TODO — consumers read state via network_manager_get_algo_result_state().
 *-------------------------------------------------------------*/

/* Copy a cJSON string field into a fixed buffer with truncation + NUL guard. */
static void algo_copy_str(cJSON *parent, const char *key, char *dst, size_t dst_size)
{
    if (dst_size == 0) {
        return;
    }
    dst[0]      = '\0';
    cJSON *item = cJSON_GetObjectItem(parent, key);
    if (cJSON_IsString(item) && item->valuestring) {
        snprintf(dst, dst_size, "%s", item->valuestring);
    }
}

static esp_err_t mcp_cb_algo_result(const void *args_json, char *response_buf, int response_buf_size)
{
    const cJSON *root = (const cJSON *)args_json;
    if (!root) {
        snprintf(response_buf, response_buf_size, "{\"ok\":false}");
        return ESP_ERR_INVALID_ARG;
    }

    /* Snapshot to fill, then publish under mutex to minimize lock hold time. */
    algo_result_state_t snap = {0};

    /* focus sub-object */
    cJSON *focus = cJSON_GetObjectItem(root, "focus");
    if (cJSON_IsObject(focus)) {
        algo_copy_str(focus, "engage_level_name", snap.engage_level_name, sizeof(snap.engage_level_name));
        algo_copy_str(focus, "focus_level_name", snap.focus_level_name, sizeof(snap.focus_level_name));
        cJSON *score = cJSON_GetObjectItem(focus, "focus_score");
        if (cJSON_IsNumber(score)) {
            snap.focus_score = (float)score->valuedouble;
        }
    }

    /* scalar fields */
    cJSON *fatigue = cJSON_GetObjectItem(root, "fatigue");
    if (cJSON_IsNumber(fatigue)) {
        snap.fatigue = fatigue->valueint;
    }
    algo_copy_str(root, "emotion", snap.emotion, sizeof(snap.emotion));
    algo_copy_str(root, "gesture", snap.gesture, sizeof(snap.gesture));

    /* vlm_game_detector sub-object */
    cJSON *vlm = cJSON_GetObjectItem(root, "vlm_game_detector");
    if (cJSON_IsObject(vlm)) {
        algo_copy_str(vlm, "judgment", snap.vlm_judgment, sizeof(snap.vlm_judgment));
        algo_copy_str(vlm, "trigger_source", snap.vlm_trigger_source, sizeof(snap.vlm_trigger_source));
        algo_copy_str(vlm, "reason", snap.vlm_reason, sizeof(snap.vlm_reason));
    }

    snap.last_update_us = esp_timer_get_time();

    /* Publish */
    if (s_algo_mutex && xSemaphoreTake(s_algo_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(&s_algo_state, &snap, sizeof(s_algo_state));
        xSemaphoreGive(s_algo_mutex);
    } else {
        ESP_LOGW(TAG, "algo_result: mutex unavailable, state not updated");
    }

    ESP_LOGI(TAG, "algo_result: emotion=%s fatigue=%d focus=%s score=%.2f gesture=%s vlm=%s|%s",
             snap.emotion[0] ? snap.emotion : "-", snap.fatigue, snap.focus_level_name[0] ? snap.focus_level_name : "-",
             snap.focus_score, snap.gesture[0] ? snap.gesture : "-", snap.vlm_judgment[0] ? snap.vlm_judgment : "-",
             snap.vlm_trigger_source[0] ? snap.vlm_trigger_source : "-");

    /*---------------------------------------------------------------
     * A1: forward VLM phone/computer detection to base board
     * Trigger: judgment=="是" AND trigger_source is 手机/电脑.
     * Throttle: same source >=60s; source change forwards immediately.
     *-------------------------------------------------------------*/
    {
        const char *source = NULL;
        if (snap.vlm_judgment[0] && strcmp(snap.vlm_judgment, "是") == 0) {
            if (strstr(snap.vlm_trigger_source, "手机")) {
                source = "phone";
            } else if (strstr(snap.vlm_trigger_source, "电脑")) {
                source = "computer";
            }
        }

        if (source) {
            int64_t now_us         = esp_timer_get_time();
            bool source_changed    = (strcmp(s_last_vlm_source, source) != 0);
            bool throttle_elapsed  = (now_us - s_last_vlm_forward_us) >= VLM_FORWARD_THROTTLE_US;
            if (source_changed || throttle_elapsed) {
                esp_err_t err = base_bridge_post_detect_phone(source);
                if (err == ESP_OK) {
                    snprintf(s_last_vlm_source, sizeof(s_last_vlm_source), "%s", source);
                    s_last_vlm_forward_us = now_us;
                } else {
                    ESP_LOGW(TAG, "VLM forward failed (will retry): source=%s", source);
                }
            } else {
                ESP_LOGI(TAG, "VLM forward throttled: source=%s", source);
            }
        }
    }

    /*---------------------------------------------------------------
     * A2: forward VLM thumb up/down gesture to base board
     * Trigger: gesture changes to Thumb_Up/Thumb_Down.
     * Throttle: >=1s between forwards.
     *-------------------------------------------------------------*/
    if (snap.gesture[0]) {
        const char *gesture = NULL;
        if (strcmp(snap.gesture, "Thumb_Up") == 0) {
            gesture = "thumb_up";
        } else if (strcmp(snap.gesture, "Thumb_Down") == 0) {
            gesture = "thumb_down";
        }

        if (gesture) {
            int64_t now_us        = esp_timer_get_time();
            bool gesture_changed  = (strcmp(s_last_gesture, gesture) != 0);
            bool throttle_elapsed = (now_us - s_last_gesture_us) >= GESTURE_FORWARD_THROTTLE_US;
            if (gesture_changed && throttle_elapsed) {
                esp_err_t err = base_bridge_post_arm_gesture(gesture);
                if (err == ESP_OK) {
                    snprintf(s_last_gesture, sizeof(s_last_gesture), "%s", gesture);
                    s_last_gesture_us = now_us;
                } else {
                    ESP_LOGW(TAG, "Gesture forward failed (will retry): gesture=%s", gesture);
                }
            }
        }
    }

    snprintf(response_buf, response_buf_size, "{\"ok\":true}");
    return ESP_OK;
}

const algo_result_state_t *network_manager_get_algo_result_state(void)
{
    return &s_algo_state;
}

/*---------------------------------------------------------------
 * REST API handlers for Expressive Eyes control
 *-------------------------------------------------------------*/
static esp_err_t rest_api_eyes_set_expression_handler(httpd_req_t *req)
{
    char buf[64] = {0};
    int ret      = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"code\":1,\"message\":\"invalid json\",\"data\":{}}");
        return ESP_FAIL;
    }

    cJSON *expr_item = cJSON_GetObjectItem(root, "expression");
    if (!cJSON_IsString(expr_item) || !expr_item->valuestring) {
        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"code\":1,\"message\":\"missing expression\",\"data\":{}}");
        return ESP_ERR_INVALID_ARG;
    }

#if CONFIG_EXAMPLE_DEMO_EXPRESSIVE_EYES
    int expr = expression_from_string(expr_item->valuestring);
    if (expr < 0) {
        cJSON_Delete(root);
        char resp[128];
        snprintf(resp, sizeof(resp), "{\"code\":1,\"message\":\"unknown expression: %s\",\"data\":{}}",
                 expr_item->valuestring);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, resp);
        return ESP_ERR_INVALID_ARG;
    }
    expressive_eyes_set_expression((expressive_eyes_expression_t)expr);
    ESP_LOGI(TAG, "REST: Eyes set_expression -> %s", expr_item->valuestring);
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"code\":0,\"message\":\"success\",\"data\":{\"expression\":\"%s\"}}",
             expr_item->valuestring);
#else
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"code\":1,\"message\":\"expressive eyes not enabled\",\"data\":{}}");
#endif
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t rest_api_eyes_look_at_handler(httpd_req_t *req)
{
    char buf[64] = {0};
    int ret      = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    float x = 0.0f, y = 0.0f;
    cJSON *root = cJSON_Parse(buf);
    if (root) {
        cJSON *x_item = cJSON_GetObjectItem(root, "x");
        cJSON *y_item = cJSON_GetObjectItem(root, "y");
        if (cJSON_IsNumber(x_item))
            x = (float)x_item->valuedouble;
        if (cJSON_IsNumber(y_item))
            y = (float)y_item->valuedouble;
        cJSON_Delete(root);
    }

    if (x < -1.0f)
        x = -1.0f;
    if (x > 1.0f)
        x = 1.0f;
    if (y < -1.0f)
        y = -1.0f;
    if (y > 1.0f)
        y = 1.0f;

#if CONFIG_EXAMPLE_DEMO_EXPRESSIVE_EYES
    expressive_eyes_look_at(x, y);
    ESP_LOGI(TAG, "REST: Eyes look_at -> (%.2f, %.2f)", x, y);
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"code\":0,\"message\":\"success\",\"data\":{\"x\":%.2f,\"y\":%.2f}}", x, y);
#else
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"code\":1,\"message\":\"expressive eyes not enabled\",\"data\":{}}");
#endif
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t rest_api_eyes_blink_handler(httpd_req_t *req)
{
#if CONFIG_EXAMPLE_DEMO_EXPRESSIVE_EYES
    expressive_eyes_blink();
    ESP_LOGI(TAG, "REST: Eyes blink");
    const char *resp = "{\"code\":0,\"message\":\"success\",\"data\":{}}";
#else
    const char *resp = "{\"code\":1,\"message\":\"expressive eyes not enabled\",\"data\":{}}";
#endif
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t rest_api_eyes_get_expression_handler(httpd_req_t *req)
{
#if CONFIG_EXAMPLE_DEMO_EXPRESSIVE_EYES
    expressive_eyes_expression_t expr = expressive_eyes_get_expression();
    int idx                           = (int)expr;
    const char *name = (idx >= 0 && idx < (int)(sizeof(s_expression_names) / sizeof(s_expression_names[0])))
                           ? s_expression_names[idx]
                           : "unknown";
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"code\":0,\"message\":\"success\",\"data\":{\"expression\":\"%s\"}}", name);
#else
    const char *resp = "{\"code\":1,\"message\":\"expressive eyes not enabled\",\"data\":{}}";
#endif
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/*---------------------------------------------------------------
 * Register REST API endpoints on the HTTP server
 *-------------------------------------------------------------*/
static void safe_register_uri(const httpd_uri_t *uri)
{
    esp_err_t ret = ws_manager_server_register_uri(uri);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Register %s failed: %s", uri->uri, esp_err_to_name(ret));
    }
}

static void register_rest_api_handlers(void)
{
    /* Camera control REST API */
    const httpd_uri_t api_camera_start = {
        .uri     = "/api/camera/start",
        .method  = HTTP_POST,
        .handler = rest_api_camera_start_handler,
    };
    safe_register_uri(&api_camera_start);

    const httpd_uri_t api_camera_stop = {
        .uri     = "/api/camera/stop",
        .method  = HTTP_POST,
        .handler = rest_api_camera_stop_handler,
    };
    safe_register_uri(&api_camera_stop);

    const httpd_uri_t api_camera_quality = {
        .uri     = "/api/camera/quality",
        .method  = HTTP_POST,
        .handler = rest_api_camera_quality_handler,
    };
    safe_register_uri(&api_camera_quality);

    const httpd_uri_t api_camera_fps = {
        .uri     = "/api/camera/fps",
        .method  = HTTP_POST,
        .handler = rest_api_camera_fps_handler,
    };
    safe_register_uri(&api_camera_fps);

    /* Display control REST API */
    const httpd_uri_t api_display_on = {
        .uri     = "/api/display/on",
        .method  = HTTP_POST,
        .handler = rest_api_display_on_handler,
    };
    safe_register_uri(&api_display_on);

    const httpd_uri_t api_display_off = {
        .uri     = "/api/display/off",
        .method  = HTTP_POST,
        .handler = rest_api_display_off_handler,
    };
    safe_register_uri(&api_display_off);

    const httpd_uri_t api_display_brightness = {
        .uri     = "/api/display/brightness",
        .method  = HTTP_POST,
        .handler = rest_api_display_brightness_handler,
    };
    safe_register_uri(&api_display_brightness);

    const httpd_uri_t api_display_camera_preview = {
        .uri     = "/api/display/camera_preview",
        .method  = HTTP_POST,
        .handler = rest_api_display_camera_preview_handler,
    };
    safe_register_uri(&api_display_camera_preview);

    /* LED control REST API */
    const httpd_uri_t api_led_on = {
        .uri     = "/api/led/on",
        .method  = HTTP_POST,
        .handler = rest_api_led_on_handler,
    };
    safe_register_uri(&api_led_on);

    const httpd_uri_t api_led_off = {
        .uri     = "/api/led/off",
        .method  = HTTP_POST,
        .handler = rest_api_led_off_handler,
    };
    safe_register_uri(&api_led_off);

    const httpd_uri_t api_led_brightness = {
        .uri     = "/api/led/brightness",
        .method  = HTTP_POST,
        .handler = rest_api_led_brightness_handler,
    };
    safe_register_uri(&api_led_brightness);

    const httpd_uri_t api_led_color_temp = {
        .uri     = "/api/led/color_temp",
        .method  = HTTP_POST,
        .handler = rest_api_led_color_temp_handler,
    };
    safe_register_uri(&api_led_color_temp);

    const httpd_uri_t api_led_status = {
        .uri     = "/api/led/status",
        .method  = HTTP_GET,
        .handler = rest_api_led_status_handler,
    };
    safe_register_uri(&api_led_status);

    /* Status endpoint */
    const httpd_uri_t api_status = {
        .uri     = "/api/status",
        .method  = HTTP_GET,
        .handler = rest_api_status_handler,
    };
    safe_register_uri(&api_status);

    /* Expressive Eyes control REST API */
    const httpd_uri_t api_eyes_set_expression = {
        .uri     = "/api/eyes/expression",
        .method  = HTTP_POST,
        .handler = rest_api_eyes_set_expression_handler,
    };
    safe_register_uri(&api_eyes_set_expression);

    const httpd_uri_t api_eyes_look_at = {
        .uri     = "/api/eyes/look_at",
        .method  = HTTP_POST,
        .handler = rest_api_eyes_look_at_handler,
    };
    safe_register_uri(&api_eyes_look_at);

    const httpd_uri_t api_eyes_blink = {
        .uri     = "/api/eyes/blink",
        .method  = HTTP_POST,
        .handler = rest_api_eyes_blink_handler,
    };
    safe_register_uri(&api_eyes_blink);

    const httpd_uri_t api_eyes_get_expression = {
        .uri     = "/api/eyes/expression",
        .method  = HTTP_GET,
        .handler = rest_api_eyes_get_expression_handler,
    };
    safe_register_uri(&api_eyes_get_expression);

    ESP_LOGI(TAG, "REST API endpoints registered: /api/camera/*, /api/display/*, /api/led/*, /api/eyes/*, /api/status");
}

/*---------------------------------------------------------------
 * Public API
 *-------------------------------------------------------------*/
esp_err_t network_manager_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 1. Initialize WiFi */
    ESP_LOGI(TAG, "Initializing WiFi...");
    s_wifi_connected_sem = xSemaphoreCreateBinary();
    if (!s_wifi_connected_sem) {
        return ESP_ERR_NO_MEM;
    }

    /* Register handlers BEFORE wifi_manager_init() because wifi_manager_init()
     * internally waits for the connection result (EventGroup bits). If we
     * register after init, the GOT_IP event will be missed and the semaphore
     * never given, causing a spurious 30s timeout. */
    wifi_manager_register_handler(WIFI_MANAGER_EVENT_GOT_IP, wifi_event_handler);
    wifi_manager_register_handler(WIFI_MANAGER_EVENT_CONNECTED, wifi_event_handler);
    wifi_manager_register_handler(WIFI_MANAGER_EVENT_DISCONNECTED, wifi_event_handler);

    esp_err_t ret = wifi_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(ret));
        vSemaphoreDelete(s_wifi_connected_sem);
        s_wifi_connected_sem = NULL;
        return ret;
    }

    /* wifi_manager_init() blocks until WiFi connects (or fails). The GOT_IP
     * callback registered above should already have fired and given the
     * semaphore. If for some reason it didn't (race condition), ensure the
     * semaphore is given when wifi_manager reports connected. Giving a
     * binary semaphore that is already given is a no-op. */
    if (wifi_manager_is_connected()) {
        xSemaphoreGive(s_wifi_connected_sem);
    }

    /* Wait for IP with timeout (should return immediately if already connected) */
    ESP_LOGI(TAG, "Waiting for IP (timeout=%dms)...", WIFI_CONNECT_TIMEOUT_MS);
    if (xSemaphoreTake(s_wifi_connected_sem, pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "WiFi connect timeout");
        vSemaphoreDelete(s_wifi_connected_sem);
        s_wifi_connected_sem = NULL;
        return ESP_ERR_TIMEOUT;
    }

    /* Get IP address */
    wifi_manager_info_t info = {0};
    if (wifi_manager_get_info(&info) == ESP_OK) {
        strncpy(s_ip_string, info.ip, sizeof(s_ip_string) - 1);
        s_ip_string[sizeof(s_ip_string) - 1] = '\0';
        ESP_LOGI(TAG, "WiFi connected, IP: %s, SSID: %s, RSSI: %d", s_ip_string, info.ssid, info.rssi);
    }

    /* 2. Initialize WebSocket server */
    ESP_LOGI(TAG, "Initializing WebSocket server...");
    ret = ws_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WS init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = ws_manager_register_handler(WS_MANAGER_EVENT_DATA, ws_data_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WS handler register failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = ws_manager_server_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WS server start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "WebSocket server running at ws://%s:80/{camera,mcp,ws,algo}", s_ip_string);

    /* 2b. Register REST API endpoints on the same HTTP server */
    register_rest_api_handlers();

    /* 3. Initialize MCP tools */
    ESP_LOGI(TAG, "Initializing MCP tools...");

    /* Create mutex guarding algorithm result state (used by algo_result callback) */
    if (s_algo_mutex == NULL) {
        s_algo_mutex = xSemaphoreCreateMutex();
        if (s_algo_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create algo_state mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    ret = mcp_tools_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MCP tools init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Register MCP tool callbacks */
    mcp_tools_callbacks_t callbacks = {
        .camera_start           = mcp_cb_camera_start,
        .camera_stop            = mcp_cb_camera_stop,
        .camera_set_quality     = mcp_cb_camera_set_quality,
        .camera_set_fps         = mcp_cb_camera_set_fps,
        .display_on             = mcp_cb_display_on,
        .display_off            = mcp_cb_display_off,
        .display_set_brightness = mcp_cb_display_set_brightness,
        .display_show_camera    = mcp_cb_display_show_camera,
        .led_on                 = mcp_cb_led_on,
        .led_off                = mcp_cb_led_off,
        .led_set_brightness     = mcp_cb_led_set_brightness,
        .led_set_color_temp     = mcp_cb_led_set_color_temp,
        .eyes_set_expression    = mcp_cb_eyes_set_expression,
        .eyes_look_at           = mcp_cb_eyes_look_at,
        .eyes_blink             = mcp_cb_eyes_blink,
        .eyes_get_expression    = mcp_cb_eyes_get_expression,
        .algo_result            = mcp_cb_algo_result,
    };
    ret = mcp_tools_register_callbacks(&callbacks);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MCP callbacks register failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 4. Initialize camera stream (only if camera is enabled) */
#if CONFIG_EXAMPLE_ENABLE_CAMERA
    camera_handles_t *cam_handles = system_manager_get_camera_handles();
    if (cam_handles != NULL) {
        ESP_LOGI(TAG, "Initializing camera stream...");
        camera_stream_config_t stream_cfg = {
            .camera          = cam_handles,
            .default_quality = 15,
            .default_fps     = 15,
            .task_stack_size = 8192,
            .task_priority   = 5,
        };
        ret = camera_stream_init(&stream_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Camera stream init failed: %s", esp_err_to_name(ret));
            /* Non-fatal: camera stream can be initialized later */
        }
    } else {
        ESP_LOGW(TAG, "Camera not initialized, skipping stream init");
    }
#endif

    /* 5. Initialize Connection Manager (WiFi + WebSocket health monitoring) */
    ESP_LOGI(TAG, "Initializing connection manager...");
    conn_mgr_config_t conn_cfg = {
        .wifi_init_timeout_ms        = WIFI_CONNECT_TIMEOUT_MS,
        .wifi_reconnect_max_delay_ms = 60000,
        .ws_reconnect_max_delay_ms   = 60000,
        .monitor_interval_ms         = 5000,
    };
    ret = connection_manager_init(&conn_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Connection manager init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = connection_manager_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Connection manager start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 6. Initialize Heartbeat Service (WebSocket server client liveness) */
    ESP_LOGI(TAG, "Starting heartbeat service...");
    ret = heartbeat_service_start_server(30, 90);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Heartbeat service start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 7. Initialize Message Queue (reliable WebSocket message delivery) */
    ESP_LOGI(TAG, "Initializing message queue...");
    ret = message_queue_init(16, 3);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Message queue init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Network manager initialized successfully");
    ESP_LOGI(TAG, "Open http://%s/ in browser or use tools/web_ui/index.html", s_ip_string);
    return ESP_OK;
}

void network_manager_deinit(void)
{
    if (!s_initialized) {
        return;
    }

    /* Stop enhanced services in reverse init order */
    message_queue_deinit();
    heartbeat_service_stop();
    connection_manager_stop();
    connection_manager_deinit();

#if CONFIG_EXAMPLE_ENABLE_CAMERA
    camera_stream_deinit();
#endif
    ws_manager_server_stop();
    ws_manager_deinit();
    wifi_manager_disconnect();
    wifi_manager_deinit();

    if (s_wifi_connected_sem) {
        vSemaphoreDelete(s_wifi_connected_sem);
        s_wifi_connected_sem = NULL;
    }

    s_initialized  = false;
    s_ip_string[0] = '\0';
    ESP_LOGI(TAG, "Network manager deinitialized");
}

esp_err_t network_manager_start_camera_stream(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return camera_stream_start();
}

esp_err_t network_manager_stop_camera_stream(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return camera_stream_stop();
}

const char *network_manager_get_ip(void)
{
    return s_ip_string;
}
