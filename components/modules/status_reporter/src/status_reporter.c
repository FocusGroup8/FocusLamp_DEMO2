/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "status_reporter.h"
#include "status_reporter_config.h"

#if (STATUS_REPORTER_ENABLE == 1)

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "device_state.h"
#include "system_config.h"
#include "lcd_module.h"

static const char *TAG = "STATUS_TX";

/*---------------------------------------------------------------
 * State
 *-------------------------------------------------------------*/
static bool s_initialized = false;
static char s_target_ip[48] = {0};
static SemaphoreHandle_t s_send_mutex = NULL;
static esp_timer_handle_t s_periodic_timer = NULL;
static volatile uint32_t s_last_send_time_ms = 0;

/*---------------------------------------------------------------
 * Helpers
 *---------------------------------------------------------------*/
static uint32_t get_millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static const char *expression_to_string(lcd_expression_t expr)
{
    static const char *names[] = {
        "normal", "happy", "sad", "angry", "surprised", "sleepy"
    };
    if (expr < sizeof(names) / sizeof(names[0])) {
        return names[expr];
    }
    return "unknown";
}

static const char *event_type_to_string(status_event_type_t event_type)
{
    switch (event_type) {
        case STATUS_EVENT_LED:     return "led";
        case STATUS_EVENT_SERVO:   return "servo";
        case STATUS_EVENT_LCD:     return "lcd";
        case STATUS_EVENT_DISPLAY: return "display";
        default:                   return "none";
    }
}

/*---------------------------------------------------------------
 * JSON building
 *-------------------------------------------------------------*/
static int build_status_json(char *buf, int buf_size, const char *trigger,
                             status_event_type_t event_type)
{
    device_state_t state;
    if (device_state_get(&state) != ESP_OK) {
        memset(&state, 0, sizeof(state));
    }

    /* Get LCD module status */
    lcd_page_t page = lcd_module_get_current_page();
    lcd_expression_t expr = lcd_module_get_expression();
    bool auto_blink = lcd_module_is_auto_blink();

    /* WiFi RSSI - not available without esp_wifi dependency (ESP-Hosted conflict) */
    int rssi = 0;

    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t uptime = state.uptime_ms / 1000;

    /* LED state from device_state */
    bool led_on = state.light.on;
    uint8_t led_brightness = state.light.brightness;
    uint8_t led_mode = state.light.level;
    uint8_t led_r = state.light.r;
    uint8_t led_g = state.light.g;
    uint8_t led_b = state.light.b;

    /* Servo state */
    int16_t em3_pos = state.servo.em3_pos;
    int16_t lx0 = state.servo.lx_pos[0];
    int16_t lx1 = state.servo.lx_pos[1];
    int16_t lx2 = state.servo.lx_pos[2];
    int16_t lx3 = state.servo.lx_pos[3];

    /* Radar state */
    bool radar_present = state.radar.present;
    float hr = state.radar.heart_rate_bpm;
    float br = state.radar.breath_rate_bpm;
    float dist = state.radar.distance_cm;

    /* Display state (placeholder - lcd_driver doesn't expose getters) */
    bool display_on = true;
    uint8_t display_brightness = 0;

    return snprintf(buf, buf_size,
             "{\"device\":\"%s\",\"version\":\"%d.%d.%d\","
             "\"uptime\":%lu,"
             "\"trigger\":\"%s\",\"event_type\":\"%s\","
             "\"led\":{\"on\":%s,\"brightness\":%d,\"mode\":%d,"
             "\"color\":{\"r\":%d,\"g\":%d,\"b\":%d}},"
             "\"display\":{\"on\":%s,\"brightness\":%d},"
             "\"lcd\":{\"page\":%d,\"expression\":\"%s\",\"auto_blink\":%s},"
             "\"servo\":{\"em3_pos\":%d,\"lx_pos\":[%d,%d,%d,%d]},"
             "\"radar\":{\"present\":%s,\"heart_rate_bpm\":%.1f,"
             "\"breath_rate_bpm\":%.1f,\"distance_cm\":%.1f},"
             "\"system\":{\"free_heap\":%lu,\"wifi_rssi\":%d,\"uptime\":%lu}}",
             PROJECT_NAME,
             SYSTEM_VERSION_MAJOR, SYSTEM_VERSION_MINOR, SYSTEM_VERSION_PATCH,
             (unsigned long)uptime,
             trigger ? trigger : "unknown",
             event_type_to_string(event_type),
             led_on ? "true" : "false",
             led_brightness,
             led_mode,
             led_r, led_g, led_b,
             display_on ? "true" : "false",
             display_brightness,
             (int)page,
             expression_to_string(expr),
             auto_blink ? "true" : "false",
             em3_pos, lx0, lx1, lx2, lx3,
             radar_present ? "true" : "false",
             hr, br, dist,
             (unsigned long)free_heap,
             rssi,
             (unsigned long)uptime);
}

/*---------------------------------------------------------------
 * HTTP POST
 *-------------------------------------------------------------*/
static esp_err_t send_status_post(const char *json_body)
{
    if (s_target_ip[0] == '\0' || strcmp(s_target_ip, "0.0.0.0") == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    char url[128];
    snprintf(url, sizeof(url), "http://%s%s", s_target_ip, STATUS_REPORTER_ENDPOINT_URI);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = STATUS_REPORTER_HTTP_TIMEOUT_MS,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_body, strlen(json_body));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        if (status_code != 200) {
            ESP_LOGW(TAG, "POST %s -> HTTP %d", STATUS_REPORTER_ENDPOINT_URI, status_code);
            err = ESP_ERR_INVALID_RESPONSE;
        }
    } else {
        ESP_LOGW(TAG, "POST %s failed: %s", STATUS_REPORTER_ENDPOINT_URI, esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return err;
}

/*---------------------------------------------------------------
 * Core send function
 *-------------------------------------------------------------*/
static esp_err_t do_send(const char *trigger, status_event_type_t event_type)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Take mutex to prevent concurrent sends */
    if (xSemaphoreTake(s_send_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Send mutex timeout, skipping report");
        return ESP_ERR_TIMEOUT;
    }

    char *json_buf = malloc(STATUS_REPORTER_JSON_BUF_SIZE);
    if (!json_buf) {
        xSemaphoreGive(s_send_mutex);
        return ESP_ERR_NO_MEM;
    }

    int len = build_status_json(json_buf, STATUS_REPORTER_JSON_BUF_SIZE, trigger, event_type);
    if (len <= 0 || len >= STATUS_REPORTER_JSON_BUF_SIZE) {
        ESP_LOGE(TAG, "JSON build failed (len=%d)", len);
        free(json_buf);
        xSemaphoreGive(s_send_mutex);
        return ESP_FAIL;
    }

    s_last_send_time_ms = get_millis();

    esp_err_t ret = send_status_post(json_buf);
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "Status reported (%s, %d bytes)", trigger, len);
    }

    free(json_buf);
    xSemaphoreGive(s_send_mutex);
    return ret;
}

/*---------------------------------------------------------------
 * Periodic timer callback
 *-------------------------------------------------------------*/
static void periodic_timer_callback(void *arg)
{
    do_send("timer", STATUS_EVENT_NONE);
}

/*---------------------------------------------------------------
 * Public API
 *-------------------------------------------------------------*/
esp_err_t status_reporter_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Status reporter already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    snprintf(s_target_ip, sizeof(s_target_ip), "%s", STATUS_REPORTER_TARGET_IP);

    if (s_target_ip[0] == '\0' || strcmp(s_target_ip, "0.0.0.0") == 0) {
        ESP_LOGW(TAG, "Target IP not configured (\"%s\"), reporting disabled", s_target_ip);
    } else {
        ESP_LOGI(TAG, "Status reporter initialized, target: %s%s", s_target_ip, STATUS_REPORTER_ENDPOINT_URI);
    }

    s_send_mutex = xSemaphoreCreateMutex();
    if (!s_send_mutex) {
        ESP_LOGE(TAG, "Failed to create send mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Create periodic timer (10 seconds) */
    const esp_timer_create_args_t timer_args = {
        .callback = &periodic_timer_callback,
        .arg = NULL,
        .name = "status_reporter_timer",
    };

    esp_err_t ret = esp_timer_create(&timer_args, &s_periodic_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timer: %s", esp_err_to_name(ret));
        vSemaphoreDelete(s_send_mutex);
        s_send_mutex = NULL;
        return ret;
    }

    /* Start periodic timer */
    ret = esp_timer_start_periodic(s_periodic_timer, STATUS_REPORTER_PERIODIC_INTERVAL_MS * 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start timer: %s", esp_err_to_name(ret));
        esp_timer_delete(s_periodic_timer);
        s_periodic_timer = NULL;
        vSemaphoreDelete(s_send_mutex);
        s_send_mutex = NULL;
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Status reporter started (interval: %d ms)", STATUS_REPORTER_PERIODIC_INTERVAL_MS);
    return ESP_OK;
}

void status_reporter_deinit(void)
{
    if (!s_initialized) {
        return;
    }

    if (s_periodic_timer) {
        esp_timer_stop(s_periodic_timer);
        esp_timer_delete(s_periodic_timer);
        s_periodic_timer = NULL;
    }

    if (s_send_mutex) {
        vSemaphoreDelete(s_send_mutex);
        s_send_mutex = NULL;
    }

    s_target_ip[0] = '\0';
    s_initialized = false;
    ESP_LOGI(TAG, "Status reporter deinitialized");
}

void status_reporter_notify_event(status_event_type_t event_type)
{
    if (!s_initialized) {
        return;
    }

    /* Debounce: skip if last send was too recent */
    uint32_t now = get_millis();
    uint32_t elapsed = now - s_last_send_time_ms;
    if (elapsed < STATUS_REPORTER_EVENT_DEBOUNCE_MS) {
        ESP_LOGD(TAG, "Event %d debounced (elapsed=%lu ms)", event_type, (unsigned long)elapsed);
        return;
    }

    ESP_LOGD(TAG, "Event triggered: %s", event_type_to_string(event_type));
    do_send("event", event_type);
}

esp_err_t status_reporter_send_now(const char *trigger)
{
    return do_send(trigger ? trigger : "manual", STATUS_EVENT_NONE);
}

#endif /* STATUS_REPORTER_ENABLE */
