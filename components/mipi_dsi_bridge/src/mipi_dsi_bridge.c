/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "mipi_dsi_bridge.h"
#include "mipi_dsi_bridge_config.h"

#include "esp_mcp_engine.h"
#include "esp_mcp_property.h"
#include "esp_mcp_tool.h"
#include "esp_mcp_data.h"

static const char *TAG = "MIPI_BRIDGE";

/*---------------------------------------------------------------
 * CRC16-CCITT for data integrity verification
 *-------------------------------------------------------------*/
static uint16_t crc16_ccitt(const uint8_t *data, int len)
{
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

/*---------------------------------------------------------------
 * State
 *-------------------------------------------------------------*/
static bool s_initialized = false;
static char s_target_ip[48] = {0};  /* IP:port buffer */

/*---------------------------------------------------------------
 * Forward declarations for MCP tool callbacks
 *-------------------------------------------------------------*/
static esp_mcp_value_t mcp_tool_camera_start(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t mcp_tool_camera_stop(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t mcp_tool_camera_set_quality(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t mcp_tool_camera_set_fps(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t mcp_tool_display_on(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t mcp_tool_display_off(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t mcp_tool_display_set_brightness(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t mcp_tool_display_show_camera(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t mcp_tool_led_on(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t mcp_tool_led_off(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t mcp_tool_led_set_brightness(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t mcp_tool_led_set_color_temp(const esp_mcp_property_list_t *properties);

/*---------------------------------------------------------------
 * HTTP client helper: send POST to mipi_dsi REST API
 *
 * Sends a POST request with optional JSON body to the target
 * endpoint, reads the response into resp_buf.
 *
 * @param path       REST API path (e.g., "/api/camera/start")
 * @param post_body  JSON body string (NULL for no body)
 * @param resp_buf   Response buffer (output, can be NULL)
 * @param resp_size  Response buffer size
 * @return ESP_OK on HTTP 200, error code otherwise
 *-------------------------------------------------------------*/
static esp_err_t http_post(const char *path, const char *post_body,
                           char *resp_buf, int resp_size)
{
    if (!s_initialized || s_target_ip[0] == '\0') {
        ESP_LOGE(TAG, "Bridge not initialized or target IP not set");
        return ESP_ERR_INVALID_STATE;
    }

    char url[128];
    snprintf(url, sizeof(url), "http://%s%s", s_target_ip, path);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = MIPI_DSI_BRIDGE_HTTP_TIMEOUT_MS,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to create HTTP client for %s", url);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");

    if (post_body) {
        esp_http_client_set_post_field(client, post_body, strlen(post_body));
        /* CRC16 checksum of request body for integrity verification */
        uint16_t crc = crc16_ccitt((const uint8_t *)post_body, strlen(post_body));
        char crc_hdr[8];
        snprintf(crc_hdr, sizeof(crc_hdr), "%04X", crc);
        esp_http_client_set_header(client, "X-CRC16", crc_hdr);
    }

    esp_err_t err = esp_http_client_perform(client);
    int status_code = 0;

    if (err == ESP_OK) {
        status_code = esp_http_client_get_status_code(client);
        int content_length = esp_http_client_get_content_length(client);

        if (resp_buf && resp_size > 0 && content_length > 0) {
            int read_len = esp_http_client_read(client, resp_buf, resp_size - 1);
            if (read_len >= 0) {
                resp_buf[read_len] = '\0';
                /* Verify response CRC16 if X-CRC16 header is present */
                char *resp_crc_val = NULL;
                if (esp_http_client_get_header(client, "X-CRC16", &resp_crc_val) == ESP_OK &&
                    resp_crc_val != NULL) {
                    uint16_t recv_crc = (uint16_t)strtol(resp_crc_val, NULL, 16);
                    uint16_t calc_crc = crc16_ccitt((const uint8_t *)resp_buf, read_len);
                    if (recv_crc != calc_crc) {
                        ESP_LOGW(TAG, "Response CRC mismatch: recv=%04X calc=%04X", recv_crc, calc_crc);
                    }
                }
            }
        }

        if (status_code != 200) {
            ESP_LOGW(TAG, "HTTP POST %s -> %d", path, status_code);
            err = ESP_ERR_INVALID_RESPONSE;
        }
    } else {
        ESP_LOGW(TAG, "HTTP POST %s failed: %s", path, esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return err;
}

/*---------------------------------------------------------------
 * HTTP GET helper for ping/status check
 *-------------------------------------------------------------*/
static esp_err_t http_get(const char *path, char *resp_buf, int resp_size)
{
    if (!s_initialized || s_target_ip[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    char url[128];
    snprintf(url, sizeof(url), "http://%s%s", s_target_ip, path);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = MIPI_DSI_BRIDGE_HTTP_TIMEOUT_MS,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status_code = 0;

    if (err == ESP_OK) {
        status_code = esp_http_client_get_status_code(client);
        int content_length = esp_http_client_get_content_length(client);

        if (resp_buf && resp_size > 0 && content_length > 0) {
            int read_len = esp_http_client_read(client, resp_buf, resp_size - 1);
            if (read_len >= 0) {
                resp_buf[read_len] = '\0';
            }
        }

        if (status_code != 200) {
            ESP_LOGW(TAG, "HTTP GET %s -> %d", path, status_code);
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }

    esp_http_client_cleanup(client);
    return err;
}

/*---------------------------------------------------------------
 * MCP tool callbacks - Camera control
 *-------------------------------------------------------------*/
static esp_mcp_value_t mcp_tool_camera_start(const esp_mcp_property_list_t *properties)
{
    (void)properties;
    ESP_LOGI(TAG, "[MCP] mipi_dsi.camera.start");

    char resp[64] = {0};
    esp_err_t ret = http_post("/api/camera/start", NULL, resp, sizeof(resp));

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Camera start response: %s", resp);
        return esp_mcp_value_create_bool(true);
    }
    ESP_LOGW(TAG, "Camera start failed: %s", esp_err_to_name(ret));
    return esp_mcp_value_create_bool(false);
}

static esp_mcp_value_t mcp_tool_camera_stop(const esp_mcp_property_list_t *properties)
{
    (void)properties;
    ESP_LOGI(TAG, "[MCP] mipi_dsi.camera.stop");

    char resp[64] = {0};
    esp_err_t ret = http_post("/api/camera/stop", NULL, resp, sizeof(resp));

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Camera stop response: %s", resp);
        return esp_mcp_value_create_bool(true);
    }
    return esp_mcp_value_create_bool(false);
}

static esp_mcp_value_t mcp_tool_camera_set_quality(const esp_mcp_property_list_t *properties)
{
    int quality = esp_mcp_property_list_get_property_int(properties, "quality");
    ESP_LOGI(TAG, "[MCP] mipi_dsi.camera.set_quality: %d", quality);

    char body[32];
    snprintf(body, sizeof(body), "{\"quality\":%d}", quality);

    char resp[64] = {0};
    esp_err_t ret = http_post("/api/camera/quality", body, resp, sizeof(resp));

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Set quality response: %s", resp);
        return esp_mcp_value_create_bool(true);
    }
    return esp_mcp_value_create_bool(false);
}

static esp_mcp_value_t mcp_tool_camera_set_fps(const esp_mcp_property_list_t *properties)
{
    int fps = esp_mcp_property_list_get_property_int(properties, "fps");
    ESP_LOGI(TAG, "[MCP] mipi_dsi.camera.set_fps: %d", fps);

    char body[32];
    snprintf(body, sizeof(body), "{\"fps\":%d}", fps);

    char resp[64] = {0};
    esp_err_t ret = http_post("/api/camera/fps", body, resp, sizeof(resp));

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Set fps response: %s", resp);
        return esp_mcp_value_create_bool(true);
    }
    return esp_mcp_value_create_bool(false);
}

/*---------------------------------------------------------------
 * MCP tool callbacks - Display control
 *-------------------------------------------------------------*/
static esp_mcp_value_t mcp_tool_display_on(const esp_mcp_property_list_t *properties)
{
    (void)properties;
    ESP_LOGI(TAG, "[MCP] mipi_dsi.display.on");

    char resp[64] = {0};
    esp_err_t ret = http_post("/api/display/on", NULL, resp, sizeof(resp));

    if (ret == ESP_OK) {
        return esp_mcp_value_create_bool(true);
    }
    return esp_mcp_value_create_bool(false);
}

static esp_mcp_value_t mcp_tool_display_off(const esp_mcp_property_list_t *properties)
{
    (void)properties;
    ESP_LOGI(TAG, "[MCP] mipi_dsi.display.off");

    char resp[64] = {0};
    esp_err_t ret = http_post("/api/display/off", NULL, resp, sizeof(resp));

    if (ret == ESP_OK) {
        return esp_mcp_value_create_bool(true);
    }
    return esp_mcp_value_create_bool(false);
}

static esp_mcp_value_t mcp_tool_display_set_brightness(const esp_mcp_property_list_t *properties)
{
    int brightness = esp_mcp_property_list_get_property_int(properties, "brightness");
    ESP_LOGI(TAG, "[MCP] mipi_dsi.display.set_brightness: %d", brightness);

    char body[32];
    snprintf(body, sizeof(body), "{\"level\":%d}", brightness);

    char resp[64] = {0};
    esp_err_t ret = http_post("/api/display/brightness", body, resp, sizeof(resp));

    if (ret == ESP_OK) {
        return esp_mcp_value_create_bool(true);
    }
    return esp_mcp_value_create_bool(false);
}

static esp_mcp_value_t mcp_tool_display_show_camera(const esp_mcp_property_list_t *properties)
{
    (void)properties;
    ESP_LOGI(TAG, "[MCP] mipi_dsi.display.show_camera");

    char resp[64] = {0};
    esp_err_t ret = http_post("/api/display/camera_preview", NULL, resp, sizeof(resp));

    if (ret == ESP_OK) {
        return esp_mcp_value_create_bool(true);
    }
    return esp_mcp_value_create_bool(false);
}

/*---------------------------------------------------------------
 * MCP tool callbacks - LED control
 *-------------------------------------------------------------*/
static esp_mcp_value_t mcp_tool_led_on(const esp_mcp_property_list_t *properties)
{
    int brightness = esp_mcp_property_list_get_property_int(properties, "brightness");
    int color_temp = esp_mcp_property_list_get_property_int(properties, "color_temp");

    /* Default: when called without parameters, brightness defaults to 100 (full on).
     * esp_mcp_property_list_get_property_int returns 0 for unset properties,
     * so brightness=0 means "not specified" for an "on" command — override to 100. */
    if (brightness <= 0) {
        brightness = 100;
    }

    ESP_LOGI(TAG, "[MCP] mipi_dsi.led.on: brightness=%d, color_temp=%d", brightness, color_temp);

    char body[64];
    if (color_temp > 0) {
        snprintf(body, sizeof(body), "{\"brightness\":%d,\"color_temp\":%d}", brightness, color_temp);
    } else {
        snprintf(body, sizeof(body), "{\"brightness\":%d}", brightness);
    }

    char resp[64] = {0};
    esp_err_t ret = http_post("/api/led/on", body, resp, sizeof(resp));

    if (ret == ESP_OK) {
        return esp_mcp_value_create_bool(true);
    }
    return esp_mcp_value_create_bool(false);
}

static esp_mcp_value_t mcp_tool_led_off(const esp_mcp_property_list_t *properties)
{
    (void)properties;
    ESP_LOGI(TAG, "[MCP] mipi_dsi.led.off");

    char resp[64] = {0};
    esp_err_t ret = http_post("/api/led/off", NULL, resp, sizeof(resp));

    if (ret == ESP_OK) {
        return esp_mcp_value_create_bool(true);
    }
    return esp_mcp_value_create_bool(false);
}

static esp_mcp_value_t mcp_tool_led_set_brightness(const esp_mcp_property_list_t *properties)
{
    int brightness = esp_mcp_property_list_get_property_int(properties, "brightness");

    /* Default to 100 if not specified */
    if (brightness <= 0) {
        brightness = 100;
    }

    ESP_LOGI(TAG, "[MCP] mipi_dsi.led.set_brightness: %d", brightness);

    char body[32];
    snprintf(body, sizeof(body), "{\"brightness\":%d}", brightness);

    char resp[64] = {0};
    esp_err_t ret = http_post("/api/led/brightness", body, resp, sizeof(resp));

    if (ret == ESP_OK) {
        return esp_mcp_value_create_bool(true);
    }
    return esp_mcp_value_create_bool(false);
}

static esp_mcp_value_t mcp_tool_led_set_color_temp(const esp_mcp_property_list_t *properties)
{
    int color_temp = esp_mcp_property_list_get_property_int(properties, "color_temp");

    /* Default to 50 (neutral) if not specified */
    if (color_temp <= 0) {
        color_temp = 50;
    }

    ESP_LOGI(TAG, "[MCP] mipi_dsi.led.set_color_temp: %d", color_temp);

    char body[32];
    snprintf(body, sizeof(body), "{\"color_temp\":%d}", color_temp);

    char resp[64] = {0};
    esp_err_t ret = http_post("/api/led/color_temp", body, resp, sizeof(resp));

    if (ret == ESP_OK) {
        return esp_mcp_value_create_bool(true);
    }
    return esp_mcp_value_create_bool(false);
}

/*---------------------------------------------------------------
 * MCP tool registration
 *-------------------------------------------------------------*/
#if (MIPI_DSI_BRIDGE_ENABLE == 1)

esp_err_t mipi_dsi_bridge_register_mcp_tools(esp_mcp_t *mcp)
{
    ESP_RETURN_ON_FALSE(mcp, ESP_ERR_INVALID_ARG, TAG, "Invalid MCP engine");

    /* self.mipi_dsi.camera.start */
    esp_mcp_tool_t *cam_start = esp_mcp_tool_create(
        "self.mipi_dsi.camera.start",
        "启动MIPI-DSI摄像头视频流",
        mcp_tool_camera_start);
    if (!cam_start) { return ESP_ERR_NO_MEM; }
    esp_mcp_add_tool(mcp, cam_start);

    /* self.mipi_dsi.camera.stop */
    esp_mcp_tool_t *cam_stop = esp_mcp_tool_create(
        "self.mipi_dsi.camera.stop",
        "停止MIPI-DSI摄像头视频流",
        mcp_tool_camera_stop);
    if (!cam_stop) { return ESP_ERR_NO_MEM; }
    esp_mcp_add_tool(mcp, cam_stop);

    /* self.mipi_dsi.camera.set_quality */
    esp_mcp_tool_t *cam_quality = esp_mcp_tool_create(
        "self.mipi_dsi.camera.set_quality",
        "设置MIPI-DSI摄像头JPEG质量 (1-100)",
        mcp_tool_camera_set_quality);
    if (!cam_quality) { return ESP_ERR_NO_MEM; }
    esp_mcp_property_t *q_prop = esp_mcp_property_create_with_range("quality", 1, 100);
    esp_mcp_tool_add_property(cam_quality, q_prop);
    esp_mcp_add_tool(mcp, cam_quality);

    /* self.mipi_dsi.camera.set_fps */
    esp_mcp_tool_t *cam_fps = esp_mcp_tool_create(
        "self.mipi_dsi.camera.set_fps",
        "设置MIPI-DSI摄像头帧率 (1-30)",
        mcp_tool_camera_set_fps);
    if (!cam_fps) { return ESP_ERR_NO_MEM; }
    esp_mcp_property_t *fps_prop = esp_mcp_property_create_with_range("fps", 1, 30);
    esp_mcp_tool_add_property(cam_fps, fps_prop);
    esp_mcp_add_tool(mcp, cam_fps);

    /* self.mipi_dsi.display.on */
    esp_mcp_tool_t *disp_on = esp_mcp_tool_create(
        "self.mipi_dsi.display.on",
        "开启MIPI-DSI显示屏",
        mcp_tool_display_on);
    if (!disp_on) { return ESP_ERR_NO_MEM; }
    esp_mcp_add_tool(mcp, disp_on);

    /* self.mipi_dsi.display.off */
    esp_mcp_tool_t *disp_off = esp_mcp_tool_create(
        "self.mipi_dsi.display.off",
        "关闭MIPI-DSI显示屏",
        mcp_tool_display_off);
    if (!disp_off) { return ESP_ERR_NO_MEM; }
    esp_mcp_add_tool(mcp, disp_off);

    /* self.mipi_dsi.display.set_brightness */
    esp_mcp_tool_t *disp_bright = esp_mcp_tool_create(
        "self.mipi_dsi.display.set_brightness",
        "设置MIPI-DSI显示屏亮度 (0-100)",
        mcp_tool_display_set_brightness);
    if (!disp_bright) { return ESP_ERR_NO_MEM; }
    esp_mcp_property_t *b_prop = esp_mcp_property_create_with_range("brightness", 0, 100);
    esp_mcp_tool_add_property(disp_bright, b_prop);
    esp_mcp_add_tool(mcp, disp_bright);

    /* self.mipi_dsi.display.show_camera */
    esp_mcp_tool_t *disp_cam = esp_mcp_tool_create(
        "self.mipi_dsi.display.show_camera",
        "在MIPI-DSI显示屏上显示摄像头预览",
        mcp_tool_display_show_camera);
    if (!disp_cam) { return ESP_ERR_NO_MEM; }
    esp_mcp_add_tool(mcp, disp_cam);

    /* self.mipi_dsi.led.on */
    esp_mcp_tool_t *led_on = esp_mcp_tool_create(
        "self.mipi_dsi.led.on",
        "开启MIPI-DSI板载LED (可指定亮度和色温)",
        mcp_tool_led_on);
    if (!led_on) { return ESP_ERR_NO_MEM; }
    esp_mcp_property_t *led_bright_prop = esp_mcp_property_create_with_range("brightness", 0, 100);
    esp_mcp_tool_add_property(led_on, led_bright_prop);
    esp_mcp_property_t *led_ct_prop = esp_mcp_property_create_with_range("color_temp", 0, 100);
    esp_mcp_tool_add_property(led_on, led_ct_prop);
    esp_mcp_add_tool(mcp, led_on);

    /* self.mipi_dsi.led.off */
    esp_mcp_tool_t *led_off = esp_mcp_tool_create(
        "self.mipi_dsi.led.off",
        "关闭MIPI-DSI板载LED",
        mcp_tool_led_off);
    if (!led_off) { return ESP_ERR_NO_MEM; }
    esp_mcp_add_tool(mcp, led_off);

    /* self.mipi_dsi.led.set_brightness */
    esp_mcp_tool_t *led_set_bright = esp_mcp_tool_create(
        "self.mipi_dsi.led.set_brightness",
        "设置MIPI-DSI板载LED亮度 (0-100)",
        mcp_tool_led_set_brightness);
    if (!led_set_bright) { return ESP_ERR_NO_MEM; }
    esp_mcp_property_t *led_sb_prop = esp_mcp_property_create_with_range("brightness", 0, 100);
    esp_mcp_tool_add_property(led_set_bright, led_sb_prop);
    esp_mcp_add_tool(mcp, led_set_bright);

    /* self.mipi_dsi.led.set_color_temp */
    esp_mcp_tool_t *led_set_ct = esp_mcp_tool_create(
        "self.mipi_dsi.led.set_color_temp",
        "设置MIPI-DSI板载LED色温 (0=最暖, 100=最冷)",
        mcp_tool_led_set_color_temp);
    if (!led_set_ct) { return ESP_ERR_NO_MEM; }
    esp_mcp_property_t *led_sct_prop = esp_mcp_property_create_with_range("color_temp", 0, 100);
    esp_mcp_tool_add_property(led_set_ct, led_sct_prop);
    esp_mcp_add_tool(mcp, led_set_ct);

    ESP_LOGI(TAG, "MIPI-DSI bridge MCP tools registered (12 tools)");
    return ESP_OK;
}

/*---------------------------------------------------------------
 * Init / Deinit
 *-------------------------------------------------------------*/

esp_err_t mipi_dsi_bridge_init(void)
{
    ESP_RETURN_ON_FALSE(!s_initialized, ESP_ERR_INVALID_STATE, TAG, "Already initialized");

    /* Read target IP from Kconfig */
    snprintf(s_target_ip, sizeof(s_target_ip), "%s", CONFIG_MIPI_DSI_BRIDGE_TARGET_IP);

    if (s_target_ip[0] == '\0' || strcmp(s_target_ip, "0.0.0.0") == 0) {
        ESP_LOGW(TAG, "Target IP not configured (\"%s\"), bridge will not be functional", s_target_ip);
    } else {
        ESP_LOGI(TAG, "MIPI-DSI bridge initialized, target: %s", s_target_ip);
    }

    s_initialized = true;
    return ESP_OK;
}

void mipi_dsi_bridge_deinit(void)
{
    if (!s_initialized) {
        return;
    }

    s_target_ip[0] = '\0';
    s_initialized = false;
    ESP_LOGI(TAG, "MIPI-DSI bridge deinitialized");
}

esp_err_t mipi_dsi_bridge_ping(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "Not initialized");

    char resp[128] = {0};
    esp_err_t ret = http_get("/api/status", resp, sizeof(resp));

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Ping successful, status: %s", resp);
    } else {
        ESP_LOGW(TAG, "Ping failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

#else /* MIPI_DSI_BRIDGE_ENABLE == 0 */

/* Stub implementations when component is disabled */

esp_err_t mipi_dsi_bridge_init(void) { return ESP_ERR_NOT_SUPPORTED; }
void mipi_dsi_bridge_deinit(void) { }
esp_err_t mipi_dsi_bridge_register_mcp_tools(esp_mcp_t *mcp) { (void)mcp; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t mipi_dsi_bridge_ping(void) { return ESP_ERR_NOT_SUPPORTED; }

#endif /* MIPI_DSI_BRIDGE_ENABLE */
