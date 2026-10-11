/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "status_receiver.h"
#include "status_receiver_config.h"

#if (STATUS_RECEIVER_ENABLE == 1)

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "websocket_manager.h"

static const char *TAG = "STATUS_RX";

/*---------------------------------------------------------------
 * State
 *-------------------------------------------------------------*/
static bool s_initialized = false;
static SemaphoreHandle_t s_mutex = NULL;
static status_receiver_data_t s_status;

/*---------------------------------------------------------------
 * Helpers
 *-------------------------------------------------------------*/
static uint32_t get_millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void lock_data(void)
{
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
}

static void unlock_data(void)
{
    if (s_mutex) {
        xSemaphoreGive(s_mutex);
    }
}

/*---------------------------------------------------------------
 * JSON parsing helpers
 *-------------------------------------------------------------*/
static void parse_led(cJSON *led_obj)
{
    if (!cJSON_IsObject(led_obj)) return;

    cJSON *on = cJSON_GetObjectItem(led_obj, "on");
    if (cJSON_IsBool(on)) {
        s_status.led.on = cJSON_IsTrue(on);
    }

    cJSON *brightness = cJSON_GetObjectItem(led_obj, "brightness");
    if (cJSON_IsNumber(brightness)) {
        s_status.led.brightness = (uint8_t)brightness->valueint;
    }

    cJSON *mode = cJSON_GetObjectItem(led_obj, "mode");
    if (cJSON_IsNumber(mode)) {
        s_status.led.mode = (uint8_t)mode->valueint;
    }

    cJSON *color = cJSON_GetObjectItem(led_obj, "color");
    if (cJSON_IsObject(color)) {
        cJSON *r = cJSON_GetObjectItem(color, "r");
        cJSON *g = cJSON_GetObjectItem(color, "g");
        cJSON *b = cJSON_GetObjectItem(color, "b");
        if (cJSON_IsNumber(r)) s_status.led.r = (uint8_t)r->valueint;
        if (cJSON_IsNumber(g)) s_status.led.g = (uint8_t)g->valueint;
        if (cJSON_IsNumber(b)) s_status.led.b = (uint8_t)b->valueint;
    }
}

static void parse_display(cJSON *display_obj)
{
    if (!cJSON_IsObject(display_obj)) return;

    cJSON *on = cJSON_GetObjectItem(display_obj, "on");
    if (cJSON_IsBool(on)) {
        s_status.display.on = cJSON_IsTrue(on);
    }

    cJSON *brightness = cJSON_GetObjectItem(display_obj, "brightness");
    if (cJSON_IsNumber(brightness)) {
        s_status.display.brightness = (uint8_t)brightness->valueint;
    }
}

static void parse_lcd(cJSON *lcd_obj)
{
    if (!cJSON_IsObject(lcd_obj)) return;

    cJSON *page = cJSON_GetObjectItem(lcd_obj, "page");
    if (cJSON_IsNumber(page)) {
        s_status.lcd.page = (uint8_t)page->valueint;
    }

    cJSON *expression = cJSON_GetObjectItem(lcd_obj, "expression");
    if (cJSON_IsString(expression) && expression->valuestring) {
        snprintf(s_status.lcd.expression, sizeof(s_status.lcd.expression),
                 "%s", expression->valuestring);
    }

    cJSON *auto_blink = cJSON_GetObjectItem(lcd_obj, "auto_blink");
    if (cJSON_IsBool(auto_blink)) {
        s_status.lcd.auto_blink = cJSON_IsTrue(auto_blink);
    }
}

static void parse_servo(cJSON *servo_obj)
{
    if (!cJSON_IsObject(servo_obj)) return;

    cJSON *em3 = cJSON_GetObjectItem(servo_obj, "em3_pos");
    if (cJSON_IsNumber(em3)) {
        s_status.servo.em3_pos = (int16_t)em3->valueint;
    }

    cJSON *lx = cJSON_GetObjectItem(servo_obj, "lx_pos");
    if (cJSON_IsArray(lx)) {
        int count = cJSON_GetArraySize(lx);
        if (count > 4) count = 4;
        for (int i = 0; i < count; i++) {
            cJSON *item = cJSON_GetArrayItem(lx, i);
            if (cJSON_IsNumber(item)) {
                s_status.servo.lx_pos[i] = (int16_t)item->valueint;
            }
        }
    }
}

static void parse_radar(cJSON *radar_obj)
{
    if (!cJSON_IsObject(radar_obj)) return;

    cJSON *present = cJSON_GetObjectItem(radar_obj, "present");
    if (cJSON_IsBool(present)) {
        s_status.radar.present = cJSON_IsTrue(present);
    }

    cJSON *hr = cJSON_GetObjectItem(radar_obj, "heart_rate_bpm");
    if (cJSON_IsNumber(hr)) {
        s_status.radar.heart_rate_bpm = (float)hr->valuedouble;
    }

    cJSON *br = cJSON_GetObjectItem(radar_obj, "breath_rate_bpm");
    if (cJSON_IsNumber(br)) {
        s_status.radar.breath_rate_bpm = (float)br->valuedouble;
    }

    cJSON *dist = cJSON_GetObjectItem(radar_obj, "distance_cm");
    if (cJSON_IsNumber(dist)) {
        s_status.radar.distance_cm = (float)dist->valuedouble;
    }
}

static void parse_ambient_light(cJSON *al_obj)
{
    if (!cJSON_IsObject(al_obj)) return;

    cJSON *level = cJSON_GetObjectItem(al_obj, "level");
    if (cJSON_IsNumber(level)) {
        s_status.ambient_light.level = (uint8_t)level->valueint;
    }

    cJSON *lux = cJSON_GetObjectItem(al_obj, "lux");
    if (cJSON_IsNumber(lux)) {
        s_status.ambient_light.lux = (float)lux->valuedouble;
    }
}

static void parse_system(cJSON *sys_obj)
{
    if (!cJSON_IsObject(sys_obj)) return;

    cJSON *free_heap = cJSON_GetObjectItem(sys_obj, "free_heap");
    if (cJSON_IsNumber(free_heap)) {
        s_status.system.free_heap = (uint32_t)free_heap->valuedouble;
    }

    cJSON *rssi = cJSON_GetObjectItem(sys_obj, "wifi_rssi");
    if (cJSON_IsNumber(rssi)) {
        s_status.system.wifi_rssi = rssi->valueint;
    }

    cJSON *uptime = cJSON_GetObjectItem(sys_obj, "uptime");
    if (cJSON_IsNumber(uptime)) {
        s_status.system.uptime = (uint32_t)uptime->valuedouble;
    }
}

/*---------------------------------------------------------------
 * HTTP handler: POST /api/status/report
 *-------------------------------------------------------------*/
static esp_err_t status_report_handler(httpd_req_t *req)
{
    char *body = malloc(STATUS_RECEIVER_MAX_BODY_SIZE);
    if (!body) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no memory\"}");
        return ESP_ERR_NO_MEM;
    }

    /* Read body */
    int total = req->content_len;
    if (total >= STATUS_RECEIVER_MAX_BODY_SIZE) {
        total = STATUS_RECEIVER_MAX_BODY_SIZE - 1;
    }
    int received = 0;
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

    lock_data();

    /* Parse top-level fields */
    cJSON *device = cJSON_GetObjectItem(root, "device");
    if (cJSON_IsString(device) && device->valuestring) {
        snprintf(s_status.device, sizeof(s_status.device), "%s", device->valuestring);
    }

    cJSON *version = cJSON_GetObjectItem(root, "version");
    if (cJSON_IsString(version) && version->valuestring) {
        snprintf(s_status.version, sizeof(s_status.version), "%s", version->valuestring);
    }

    /* Parse subsystems */
    parse_led(cJSON_GetObjectItem(root, "led"));
    parse_display(cJSON_GetObjectItem(root, "display"));
    parse_lcd(cJSON_GetObjectItem(root, "lcd"));
    parse_servo(cJSON_GetObjectItem(root, "servo"));
    parse_radar(cJSON_GetObjectItem(root, "radar"));
    parse_ambient_light(cJSON_GetObjectItem(root, "ambient_light"));
    parse_system(cJSON_GetObjectItem(root, "system"));

    s_status.valid = true;
    s_status.timestamp = get_millis();

    unlock_data();

    cJSON_Delete(root);
    free(body);

    /* Send response */
    httpd_resp_set_type(req, "application/json");
    const char *resp = "{\"ok\":true}";
    httpd_resp_send(req, resp, strlen(resp));

    ESP_LOGD(TAG, "Status report received: %d bytes", received);
    return ESP_OK;
}

static const httpd_uri_t status_report_uri = {
    .uri      = STATUS_RECEIVER_ENDPOINT_URI,
    .method   = HTTP_POST,
    .handler  = status_report_handler,
    .user_ctx = NULL,
};

/*---------------------------------------------------------------
 * Init / Deinit
 *-------------------------------------------------------------*/
esp_err_t status_receiver_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Status receiver already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    memset(&s_status, 0, sizeof(s_status));

    /* Verify server is running */
    if (!ws_manager_server_is_running()) {
        ESP_LOGE(TAG, "WebSocket server not running, cannot register URI");
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_ERR_INVALID_STATE;
    }

    /* Register URI handler */
    esp_err_t ret = ws_manager_server_register_uri(&status_report_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register status report URI: %s", esp_err_to_name(ret));
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Status receiver initialized, endpoint: %s", STATUS_RECEIVER_ENDPOINT_URI);
    return ESP_OK;
}

void status_receiver_deinit(void)
{
    if (!s_initialized) {
        return;
    }

    if (s_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }

    s_initialized = false;
    memset(&s_status, 0, sizeof(s_status));
    ESP_LOGI(TAG, "Status receiver deinitialized");
}

/*---------------------------------------------------------------
 * Getter functions
 *-------------------------------------------------------------*/
esp_err_t status_receiver_get_status(status_receiver_data_t *data)
{
    if (!s_initialized || !data) {
        return ESP_ERR_INVALID_ARG;
    }

    lock_data();
    if (!s_status.valid) {
        unlock_data();
        return ESP_ERR_NOT_FOUND;
    }
    *data = s_status;
    unlock_data();

    return ESP_OK;
}

esp_err_t status_receiver_get_json(char *buf, int buf_size)
{
    if (!buf || buf_size <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    lock_data();
    if (!s_status.valid) {
        unlock_data();
        snprintf(buf, buf_size, "{\"ok\":false,\"error\":\"no data\"}");
        return ESP_ERR_NOT_FOUND;
    }

    snprintf(buf, buf_size,
             "{\"ok\":true,\"device\":\"%s\",\"version\":\"%s\","
             "\"timestamp\":%lu,"
             "\"led\":{\"on\":%s,\"brightness\":%d,\"mode\":%d,"
             "\"color\":{\"r\":%d,\"g\":%d,\"b\":%d}},"
             "\"display\":{\"on\":%s,\"brightness\":%d},"
             "\"lcd\":{\"page\":%d,\"expression\":\"%s\",\"auto_blink\":%s},"
             "\"servo\":{\"em3_pos\":%d,\"lx_pos\":[%d,%d,%d,%d]},"
             "\"radar\":{\"present\":%s,\"heart_rate_bpm\":%.1f,"
             "\"breath_rate_bpm\":%.1f,\"distance_cm\":%.1f},"
             "\"ambient_light\":{\"level\":%d,\"lux\":%.1f},"
             "\"system\":{\"free_heap\":%lu,\"wifi_rssi\":%d,\"uptime\":%lu}}",
             s_status.device,
             s_status.version,
             (unsigned long)s_status.timestamp,
             s_status.led.on ? "true" : "false",
             s_status.led.brightness,
             s_status.led.mode,
             s_status.led.r, s_status.led.g, s_status.led.b,
             s_status.display.on ? "true" : "false",
             s_status.display.brightness,
             s_status.lcd.page,
             s_status.lcd.expression,
             s_status.lcd.auto_blink ? "true" : "false",
             s_status.servo.em3_pos,
             s_status.servo.lx_pos[0], s_status.servo.lx_pos[1],
             s_status.servo.lx_pos[2], s_status.servo.lx_pos[3],
             s_status.radar.present ? "true" : "false",
             s_status.radar.heart_rate_bpm,
             s_status.radar.breath_rate_bpm,
             s_status.radar.distance_cm,
             s_status.ambient_light.level,
             s_status.ambient_light.lux,
             (unsigned long)s_status.system.free_heap,
             s_status.system.wifi_rssi,
             (unsigned long)s_status.system.uptime);

    unlock_data();
    return ESP_OK;
}

bool status_receiver_is_fresh(void)
{
    if (!s_initialized || !s_status.valid) {
        return false;
    }

    uint32_t now = get_millis();
    uint32_t age = now - s_status.timestamp;
    return (age < STATUS_RECEIVER_STALE_TIMEOUT_MS);
}

#endif /* STATUS_RECEIVER_ENABLE */
