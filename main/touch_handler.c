/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "touch_handler.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#if CONFIG_EXAMPLE_ENABLE_LED
#include "led_controller.h"
#endif

#if CONFIG_EXAMPLE_ENABLE_DISPLAY
#include "display_system.h"
#endif

#if CONFIG_TOUCH_INTERPRETER_ENABLE
#include "touch_interpreter.h"
#include "touch_interpreter_types.h"
#endif

#include "websocket_manager.h"

#include <string.h>

static const char *TAG = "TOUCH_HANDLER";

/*---------------------------------------------------------------
 * Welcome messages (3 sets, randomly selected)
 *-------------------------------------------------------------*/
static const char *s_welcome_messages[] = {
    "你好！有什么可以帮你的？",
    "嗨！很高兴见到你！",
    "你好呀！今天过得怎么样？",
};
#define WELCOME_MSG_COUNT 3

/*---------------------------------------------------------------
 * Brightness levels: 20/40/60/80/100%
 *-------------------------------------------------------------*/
static const uint8_t s_brightness_levels[] = {20, 40, 60, 80, 100};
#define BRIGHTNESS_LEVEL_COUNT 5
#define BRIGHTNESS_DEFAULT_LEVEL 2 /* Index 2 = 60% */

/*---------------------------------------------------------------
 * Module state (forward-declared for NVS functions)
 *-------------------------------------------------------------*/
static bool s_initialized             = false;
static uint8_t s_brightness_level_idx = BRIGHTNESS_DEFAULT_LEVEL;
static bool s_wake_pending            = false; /* Flag for wifi_test to poll */
static SemaphoreHandle_t s_wake_mutex = NULL;

/*---------------------------------------------------------------
 * NVS configuration storage
 *-------------------------------------------------------------*/
#define NVS_NAMESPACE "touch_cfg"
#define NVS_KEY_BRIGHTNESS_IDX "brt_idx"

static void nvs_save_brightness_level(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed for save: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u8(handle, NVS_KEY_BRIGHTNESS_IDX, s_brightness_level_idx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS set brightness idx failed: %s", esp_err_to_name(err));
    } else {
        nvs_commit(handle);
    }
    nvs_close(handle);
}

static void nvs_load_brightness_level(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "NVS not found or not opened, using default brightness");
        return;
    }
    uint8_t saved_idx = BRIGHTNESS_DEFAULT_LEVEL;
    err               = nvs_get_u8(handle, NVS_KEY_BRIGHTNESS_IDX, &saved_idx);
    if (err == ESP_OK && saved_idx < BRIGHTNESS_LEVEL_COUNT) {
        s_brightness_level_idx = saved_idx;
        ESP_LOGI(TAG, "Loaded brightness level from NVS: index %d (%d%%)", saved_idx, s_brightness_levels[saved_idx]);
    }
    nvs_close(handle);
}

static void nvs_reset_config(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return;
    }
    nvs_erase_key(handle, NVS_KEY_BRIGHTNESS_IDX);
    nvs_commit(handle);
    nvs_close(handle);
    s_brightness_level_idx = BRIGHTNESS_DEFAULT_LEVEL;
    ESP_LOGI(TAG, "NVS config reset to defaults");
}

/*---------------------------------------------------------------
 * LED blink helper
 *
 * Blinks LED on-off-on with 100ms intervals for visual feedback.
 *-------------------------------------------------------------*/
static void led_blink_feedback(void)
{
#if CONFIG_EXAMPLE_ENABLE_LED
    if (!led_is_initialized())
        return;

    /* Blink pattern: ON(100ms) -> OFF(100ms) -> ON(100ms) */
    led_set_brightness_with_cct(80);
    vTaskDelay(pdMS_TO_TICKS(100));
    led_off();
    vTaskDelay(pdMS_TO_TICKS(100));
    led_set_brightness_with_cct(80);
    vTaskDelay(pdMS_TO_TICKS(100));
    led_off();
#else
    (void)0;
#endif
}

/*---------------------------------------------------------------
 * LED blink task (runs in task context, not from callback)
 *-------------------------------------------------------------*/
static void led_blink_task(void *arg)
{
    (void)arg;
    led_blink_feedback();
    vTaskDelete(NULL);
}

/*---------------------------------------------------------------
 * Gesture callback
 *
 * Called from touch_interpreter processing task context.
 * Safe to call most ESP-IDF APIs.
 *-------------------------------------------------------------*/
static void gesture_callback(const touch_gesture_event_t *event, void *ctx)
{
    (void)ctx;

    switch (event->gesture) {
    case TOUCH_GESTURE_TAP: {
        ESP_LOGI(TAG, "TAP detected — triggering wake sequence");

        /* 1. LED blink feedback (in separate task to not block) */
        xTaskCreate(led_blink_task, "led_blink", 2048, NULL, 5, NULL);

        /* 2. Mark wake pending for wifi_test to poll */
        if (s_wake_mutex) {
            xSemaphoreTake(s_wake_mutex, portMAX_DELAY);
            s_wake_pending = true;
            xSemaphoreGive(s_wake_mutex);
        }

        /* 3. Display welcome text on screen (TODO: integrate with LVGL) */
        ESP_LOGI(TAG, "Welcome: %s", s_welcome_messages[esp_timer_get_time() % WELCOME_MSG_COUNT]);

        /* 4. TODO: Send HTTP POST to wifi_test to trigger wake word */
        /* For now, wifi_test will poll /api/touch/wake endpoint */
        break;
    }

    case TOUCH_GESTURE_DOUBLE_TAP: {
        ESP_LOGI(TAG, "DOUBLE_TAP detected — cycling brightness");

        /* Cycle to next brightness level */
        s_brightness_level_idx    = (s_brightness_level_idx + 1) % BRIGHTNESS_LEVEL_COUNT;
        uint8_t target_brightness = s_brightness_levels[s_brightness_level_idx];

#if CONFIG_EXAMPLE_ENABLE_LED
        if (led_is_initialized()) {
            led_set_brightness_with_cct_fade(target_brightness, 300);
        }
#endif

        ESP_LOGI(TAG, "Brightness level %d: %d%%", s_brightness_level_idx + 1, target_brightness);

        /* Save brightness level to NVS */
        nvs_save_brightness_level();

        /* TODO: Display brightness number + progress bar on screen for 2 seconds */
        /* TODO: Notify wifi_test to TTS "亮度已调到百分之XX" */
        break;
    }

    case TOUCH_GESTURE_LONG_PRESS: {
        ESP_LOGI(TAG, "LONG_PRESS detected (duration=%lldms) — no action bound yet", event->duration_ms);
        /* Placeholder for future functionality */
        break;
    }

    default:
        break;
    }
}

/*---------------------------------------------------------------
 * REST API: /api/touch/wake (GET)
 *
 * Returns whether a wake event is pending.
 * wifi_test polls this endpoint; if wake_pending=true,
 * it should call xiaozhi_manager_send_wake_word() and
 * the pending flag is automatically cleared.
 *-------------------------------------------------------------*/
static esp_err_t rest_api_touch_wake_handler(httpd_req_t *req)
{
    bool pending = false;
    if (s_wake_mutex) {
        xSemaphoreTake(s_wake_mutex, portMAX_DELAY);
        pending = s_wake_pending;
        if (pending) {
            s_wake_pending = false; /* Auto-clear on read */
        }
        xSemaphoreGive(s_wake_mutex);
    }

    char resp[96];
    snprintf(resp, sizeof(resp), "{\"code\":0,\"message\":\"success\",\"data\":{\"wake_pending\":%s}}",
             pending ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/*---------------------------------------------------------------
 * REST API: /api/touch/brightness (GET)
 *
 * Returns current brightness level info.
 *-------------------------------------------------------------*/
static esp_err_t rest_api_touch_brightness_handler(httpd_req_t *req)
{
    uint8_t current = s_brightness_levels[s_brightness_level_idx];
    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"code\":0,\"message\":\"success\",\"data\":{\"level_index\":%d,\"brightness\":%d,\"total_levels\":%d}}",
             s_brightness_level_idx + 1, current, BRIGHTNESS_LEVEL_COUNT);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/*---------------------------------------------------------------
 * REST API: /api/touch/reset (POST)
 *
 * Resets all touch configuration to defaults (NVS erase).
 * Called remotely via MCP tool from wifi_test.
 *-------------------------------------------------------------*/
static esp_err_t rest_api_touch_reset_handler(httpd_req_t *req)
{
    nvs_reset_config();

#if CONFIG_EXAMPLE_ENABLE_LED
    if (led_is_initialized()) {
        led_set_brightness_with_cct(s_brightness_levels[s_brightness_level_idx]);
    }
#endif

    const char *resp = "{\"code\":0,\"message\":\"success\",\"data\":{\"brightness_level\":3,\"brightness\":60}}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/*---------------------------------------------------------------
 * Register REST API endpoints
 *-------------------------------------------------------------*/
static void register_touch_api_handlers(void)
{
    const httpd_uri_t api_touch_wake = {
        .uri     = "/api/touch/wake",
        .method  = HTTP_GET,
        .handler = rest_api_touch_wake_handler,
    };
    ws_manager_server_register_uri(&api_touch_wake);

    const httpd_uri_t api_touch_brightness = {
        .uri     = "/api/touch/brightness",
        .method  = HTTP_GET,
        .handler = rest_api_touch_brightness_handler,
    };
    ws_manager_server_register_uri(&api_touch_brightness);

    const httpd_uri_t api_touch_reset = {
        .uri     = "/api/touch/reset",
        .method  = HTTP_POST,
        .handler = rest_api_touch_reset_handler,
    };
    ws_manager_server_register_uri(&api_touch_reset);

    ESP_LOGI(TAG, "REST API endpoints registered: /api/touch/wake, /api/touch/brightness, /api/touch/reset");
}

/*---------------------------------------------------------------
 * Public API
 *-------------------------------------------------------------*/

esp_err_t touch_handler_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing touch handler...");

    /* Load saved brightness level from NVS */
    nvs_load_brightness_level();

    /* Create mutex for wake_pending flag */
    s_wake_mutex = xSemaphoreCreateMutex();
    if (!s_wake_mutex) {
        ESP_LOGE(TAG, "Failed to create wake mutex");
        return ESP_ERR_NO_MEM;
    }

#if CONFIG_TOUCH_INTERPRETER_ENABLE
    /* Initialize touch interpreter with gesture callback */
    touch_interpreter_config_t interp_cfg = TOUCH_INTERPRETER_DEFAULT_CONFIG();
    interp_cfg.gesture_cb                 = gesture_callback;
    interp_cfg.user_ctx                   = NULL;

    esp_err_t ret = touch_interpreter_init(&interp_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Touch interpreter init failed: %s", esp_err_to_name(ret));
        vSemaphoreDelete(s_wake_mutex);
        s_wake_mutex = NULL;
        return ret;
    }
#else
    ESP_LOGW(TAG, "Touch interpreter not enabled — touch handler will be non-functional");
    /* Still mark as initialized so REST API endpoints work (return defaults) */
#endif

    s_initialized = true;
    ESP_LOGI(TAG, "Touch handler initialized (brightness default: %d%%)", s_brightness_levels[s_brightness_level_idx]);
    return ESP_OK;
}

esp_err_t touch_handler_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

#if CONFIG_TOUCH_INTERPRETER_ENABLE
    touch_interpreter_deinit();
#endif

    if (s_wake_mutex) {
        vSemaphoreDelete(s_wake_mutex);
        s_wake_mutex = NULL;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "Touch handler deinitialized");
    return ESP_OK;
}

bool touch_handler_is_initialized(void)
{
    return s_initialized;
}

/* Called from network_manager_init() after HTTP server is up */
void touch_handler_register_api(void)
{
    if (s_initialized) {
        register_touch_api_handlers();
    }
}
