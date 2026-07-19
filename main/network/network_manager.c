/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "network_manager.h"

#include "cJSON.h"
#include "camera_stream.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mcp_tools.h"
#include "system_manager.h"
#include "websocket_manager.h"
#include "wifi_manager.h"

#include <string.h>

static const char *TAG = "net_mgr";

#define MCP_RESPONSE_BUF_SIZE 1024
#define WIFI_CONNECT_TIMEOUT_MS 30000

static bool s_initialized                     = false;
static SemaphoreHandle_t s_wifi_connected_sem = NULL;
static char s_ip_string[16]                   = {0};

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

    /* Only handle text messages on /mcp path */
    if (msg->type != WS_DATA_TYPE_TEXT || !msg->uri) {
        return;
    }

    if (strcmp(msg->uri, "/mcp") != 0) {
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

    /* Send response back to client */
    if (msg->client_fd >= 0 && response_buf[0] != '\0') {
        ret = ws_manager_server_send_text(msg->client_fd, response_buf, strlen(response_buf));
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "MCP response send failed: %s", esp_err_to_name(ret));
        }
    }
}

/*---------------------------------------------------------------
 * MCP Tool Callbacks: Camera control
 *-------------------------------------------------------------*/
static esp_err_t mcp_cb_camera_start(const void *args_json, char *response_buf, int response_buf_size)
{
    (void)args_json;
    esp_err_t ret = camera_stream_start();
    snprintf(response_buf, response_buf_size, "{\"ok\":%s}", (ret == ESP_OK) ? "true" : "false");
    return ret;
}

static esp_err_t mcp_cb_camera_stop(const void *args_json, char *response_buf, int response_buf_size)
{
    (void)args_json;
    esp_err_t ret = camera_stream_stop();
    snprintf(response_buf, response_buf_size, "{\"ok\":%s}", (ret == ESP_OK) ? "true" : "false");
    return ret;
}

static esp_err_t mcp_cb_camera_set_quality(const void *args_json, char *response_buf, int response_buf_size)
{
    const cJSON *args = (const cJSON *)args_json;
    if (!cJSON_IsObject(args)) {
        snprintf(response_buf, response_buf_size, "{\"error\":\"invalid args\"}");
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *q = cJSON_GetObjectItem(args, "quality");
    if (!cJSON_IsNumber(q) || q->valueint < 1 || q->valueint > 100) {
        snprintf(response_buf, response_buf_size, "{\"error\":\"quality must be 1-100\"}");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = camera_stream_set_quality(q->valueint);
    snprintf(response_buf, response_buf_size, "{\"quality\":%d}", camera_stream_get_quality());
    return ret;
}

static esp_err_t mcp_cb_camera_set_fps(const void *args_json, char *response_buf, int response_buf_size)
{
    const cJSON *args = (const cJSON *)args_json;
    if (!cJSON_IsObject(args)) {
        snprintf(response_buf, response_buf_size, "{\"error\":\"invalid args\"}");
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *f = cJSON_GetObjectItem(args, "fps");
    if (!cJSON_IsNumber(f) || f->valueint < 1 || f->valueint > 30) {
        snprintf(response_buf, response_buf_size, "{\"error\":\"fps must be 1-30\"}");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = camera_stream_set_fps(f->valueint);
    snprintf(response_buf, response_buf_size, "{\"fps\":%d}", camera_stream_get_fps());
    return ret;
}

/*---------------------------------------------------------------
 * MCP Tool Callbacks: Display control
 *-------------------------------------------------------------*/
static esp_err_t mcp_cb_display_on(const void *args_json, char *response_buf, int response_buf_size)
{
    (void)args_json;
    /* TODO: implement display on/off via display_system when API is extended.
     * For now, return ok to acknowledge command. */
    snprintf(response_buf, response_buf_size, "{\"ok\":true,\"note\":\"display_on not fully implemented\"}");
    return ESP_OK;
}

static esp_err_t mcp_cb_display_off(const void *args_json, char *response_buf, int response_buf_size)
{
    (void)args_json;
    snprintf(response_buf, response_buf_size, "{\"ok\":true,\"note\":\"display_off not fully implemented\"}");
    return ESP_OK;
}

static esp_err_t mcp_cb_display_set_brightness(const void *args_json, char *response_buf, int response_buf_size)
{
    const cJSON *args = (const cJSON *)args_json;
    if (!cJSON_IsObject(args)) {
        snprintf(response_buf, response_buf_size, "{\"error\":\"invalid args\"}");
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *level = cJSON_GetObjectItem(args, "level");
    if (!cJSON_IsNumber(level) || level->valueint < 0 || level->valueint > 100) {
        snprintf(response_buf, response_buf_size, "{\"error\":\"level must be 0-100\"}");
        return ESP_ERR_INVALID_ARG;
    }
    /* TODO: implement via display_system when API is extended */
    snprintf(response_buf, response_buf_size, "{\"level\":%d,\"note\":\"brightness not fully implemented\"}",
             level->valueint);
    return ESP_OK;
}

static esp_err_t mcp_cb_display_show_camera(const void *args_json, char *response_buf, int response_buf_size)
{
    const cJSON *args = (const cJSON *)args_json;
    if (!cJSON_IsObject(args)) {
        snprintf(response_buf, response_buf_size, "{\"error\":\"invalid args\"}");
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *enable = cJSON_GetObjectItem(args, "enable");
    if (!cJSON_IsBool(enable)) {
        snprintf(response_buf, response_buf_size, "{\"error\":\"enable must be bool\"}");
        return ESP_ERR_INVALID_ARG;
    }
    /* TODO: implement local display camera preview when display_system API is extended */
    bool en = cJSON_IsTrue(enable);
    snprintf(response_buf, response_buf_size, "{\"enable\":%s,\"note\":\"show_camera not fully implemented\"}",
             en ? "true" : "false");
    return ESP_OK;
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

    ESP_LOGI(TAG, "WebSocket server running at ws://%s:80/{camera,mcp,ws}", s_ip_string);

    /* 3. Initialize MCP tools */
    ESP_LOGI(TAG, "Initializing MCP tools...");
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
