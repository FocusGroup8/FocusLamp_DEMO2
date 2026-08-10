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

#if CONFIG_EXAMPLE_DEMO_EXPRESSIVE_EYES
#include "expressive_eyes_display.h"
#endif

#if CONFIG_TOUCH_INTERPRETER_ENABLE
#include "touch_interpreter.h"
#include "touch_interpreter_types.h"
#endif

#include "websocket_manager.h"

#include "tts_inject.h"

#include <string.h>

static const char *TAG = "TOUCH_HANDLER";

/*---------------------------------------------------------------
 * Welcome messages (3 phrase commands, alternated on each tap).
 * TTS playback only accepts short phrase triggers; the voice board (cloud)
 * maps each phrase command (欢迎语1/2/3) to the full greeting text.
 * The user must sync the corresponding cloud commands manually.
 *-------------------------------------------------------------*/
static const char *s_welcome_messages[] = {
    "欢迎语1",
    "欢迎语2",
    "欢迎语3",
};
#define WELCOME_MSG_COUNT 3
static uint8_t s_welcome_idx = 0; /* Alternating index into s_welcome_messages */

/*---------------------------------------------------------------
 * Brightness levels: fixed absolute 5-level brightness.
 * The ambient light sensor module has been removed (hardware returns a
 * fixed value), so head light brightness is no longer ambient-adjusted.
 * Double-tap cycles 20% -> 40% -> 60% -> 80% -> 100% -> 20% ...
 *-------------------------------------------------------------*/
static const uint8_t s_brightness_levels[] = {20, 40, 60, 80, 100};
#define BRIGHTNESS_LEVEL_COUNT 5
#define BRIGHTNESS_DEFAULT_LEVEL 2 /* Index 2 = 60% default */

/*---------------------------------------------------------------
 * Module state (forward-declared for NVS functions)
 *-------------------------------------------------------------*/
static bool s_initialized             = false;
static uint8_t s_brightness_level_idx = BRIGHTNESS_DEFAULT_LEVEL;
static bool s_wake_pending            = false; /* Flag for wifi_test to poll */
static SemaphoreHandle_t s_wake_mutex = NULL;

/*---------------------------------------------------------------
 * Touch action queue
 *
 * The gesture callback runs in the FreeRTOS timer service task (Tmr Svc),
 * whose stack is only ~2KB — too small for esp_http_client (asprintf etc.).
 * All HTTP-heavy work (TTS injection) is therefore deferred
 * to a dedicated task via this queue.
 *-------------------------------------------------------------*/
typedef enum {
    TOUCH_ACTION_TAP,
    TOUCH_ACTION_DOUBLE_TAP,
} touch_action_type_t;
#define TOUCH_ACTION_QUEUE_LEN 8
#define TOUCH_ACTION_TASK_STACK 8192
#define TOUCH_ACTION_TASK_PRIO 3
static QueueHandle_t s_action_queue = NULL;
static TaskHandle_t s_action_task   = NULL;

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
    err = nvs_get_u8(handle, NVS_KEY_BRIGHTNESS_IDX, &saved_idx);
    if (err == ESP_OK && saved_idx < BRIGHTNESS_LEVEL_COUNT) {
        s_brightness_level_idx = saved_idx;
        ESP_LOGI(TAG, "Loaded brightness level from NVS: index %d (offset %d%%)", saved_idx,
                 s_brightness_levels[saved_idx]);
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
 * Current absolute brightness (from the fixed brightness level table).
 * No ambient light dependency: the light sensor module was removed, so the
 * head light always uses the fixed 5-level table (contract update).
 *-------------------------------------------------------------*/
static uint8_t get_current_brightness(void)
{
    return s_brightness_levels[s_brightness_level_idx];
}

/*---------------------------------------------------------------
 * TAP action: head light on + happy expression + welcome TTS.
 * Brightness is the current fixed level (no ambient dependency).
 * Runs in the dedicated action task (adequate stack for HTTP).
 *-------------------------------------------------------------*/
static void handle_tap_action(void)
{
    ESP_LOGI(TAG, "TAP action - welcome sequence");

    /* 1. Turn on head LED at the current fixed brightness level */
    uint8_t target_brightness = get_current_brightness();
#if CONFIG_EXAMPLE_ENABLE_LED
    if (led_is_initialized()) {
        led_set_brightness_with_cct_fade(target_brightness, 300);
    }
#endif

    /* 2. Switch expression to happy */
#if CONFIG_EXAMPLE_DEMO_EXPRESSIVE_EYES
    expressive_eyes_set_expression(EXPRESSIVE_EYES_HAPPY);
#endif

    /* 3. Mark wake pending for wifi_test to poll (triggers TTS welcome) */
    if (s_wake_mutex) {
        xSemaphoreTake(s_wake_mutex, portMAX_DELAY);
        s_wake_pending = true;
        xSemaphoreGive(s_wake_mutex);
    }

    /* 4. Inject a welcome message via the voice board (alternating, §5.2) */
    const char *welcome = s_welcome_messages[s_welcome_idx];
    s_welcome_idx       = (s_welcome_idx + 1) % WELCOME_MSG_COUNT;
    ESP_LOGI(TAG, "Welcome TTS inject: \"%s\"", welcome);
    esp_err_t terr = tts_inject_speak(welcome);
    if (terr != ESP_OK) {
        ESP_LOGW(TAG, "Welcome TTS inject failed: %s", esp_err_to_name(terr));
    }
}

/*---------------------------------------------------------------
 * DOUBLE_TAP action: cycle fixed 5-level brightness (no ambient base).
 * Each double tap advances one level; after the highest level it wraps
 * around to the lowest (20% -> 100% -> 20%).
 * Runs in the dedicated action task (adequate stack for HTTP).
 *-------------------------------------------------------------*/
static void handle_double_tap_action(void)
{
    ESP_LOGI(TAG, "DOUBLE_TAP action - cycling brightness");

    /* Cycle to next brightness level (absolute value, no ambient) */
    s_brightness_level_idx = (s_brightness_level_idx + 1) % BRIGHTNESS_LEVEL_COUNT;
    uint8_t target_brightness = s_brightness_levels[s_brightness_level_idx];

#if CONFIG_EXAMPLE_ENABLE_LED
    if (led_is_initialized()) {
        led_set_brightness_with_cct_fade(target_brightness, 300);
    }
#endif

    ESP_LOGI(TAG, "Brightness level %d/%d: %d%%", s_brightness_level_idx + 1,
             BRIGHTNESS_LEVEL_COUNT, target_brightness);

    /* Save brightness level to NVS */
    nvs_save_brightness_level();

    /* TODO: Display brightness number + progress bar on screen for 2 seconds */
    /* TODO: Notify wifi_test to TTS "亮度已调到百分之XX" */
}

/*---------------------------------------------------------------
 * Action task: consumes queued touch actions.
 *-------------------------------------------------------------*/
static void touch_action_task(void *arg)
{
    (void)arg;
    touch_action_type_t action;

    while (1) {
        if (xQueueReceive(s_action_queue, &action, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (action == TOUCH_ACTION_TAP) {
            handle_tap_action();
        } else if (action == TOUCH_ACTION_DOUBLE_TAP) {
            handle_double_tap_action();
        }
    }
}

/*---------------------------------------------------------------
 * Gesture callback
 *
 * Called from touch_interpreter processing / timer service task context.
 * The timer service task has only ~2KB of stack, so only light work is
 * allowed here; HTTP-heavy actions are enqueued for the action task.
 *-------------------------------------------------------------*/
static void gesture_callback(const touch_gesture_event_t *event, void *ctx)
{
    (void)ctx;

    switch (event->gesture) {
    case TOUCH_GESTURE_TAP:
        ESP_LOGI(TAG, "TAP detected - enqueuing welcome sequence");
        if (s_action_queue) {
            touch_action_type_t action = TOUCH_ACTION_TAP;
            xQueueSend(s_action_queue, &action, 0);
        }
        break;

    case TOUCH_GESTURE_DOUBLE_TAP:
        ESP_LOGI(TAG, "DOUBLE_TAP detected - enqueuing brightness cycle");
        if (s_action_queue) {
            touch_action_type_t action = TOUCH_ACTION_DOUBLE_TAP;
            xQueueSend(s_action_queue, &action, 0);
        }
        break;

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
 * Returns current brightness level info (level index and absolute %).
 * The absolute brightness is the fixed level from the 5-level table.
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
 * Resets all touch configuration to defaults (NVS erase) and restores
 * the default fixed brightness level.
 * Called remotely via MCP tool from wifi_test.
 *-------------------------------------------------------------*/
static esp_err_t rest_api_touch_reset_handler(httpd_req_t *req)
{
    nvs_reset_config();

    uint8_t target = s_brightness_levels[s_brightness_level_idx];

#if CONFIG_EXAMPLE_ENABLE_LED
    if (led_is_initialized()) {
        led_set_brightness_with_cct(target);
    }
#endif

    char resp[96];
    snprintf(resp, sizeof(resp),
             "{\"code\":0,\"message\":\"success\",\"data\":{\"brightness_level\":%d,\"brightness\":%d}}",
             BRIGHTNESS_DEFAULT_LEVEL + 1, target);
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
    esp_err_t ret = ws_manager_server_register_uri(&api_touch_wake);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Register %s failed: %s", api_touch_wake.uri, esp_err_to_name(ret));
    }

    const httpd_uri_t api_touch_brightness = {
        .uri     = "/api/touch/brightness",
        .method  = HTTP_GET,
        .handler = rest_api_touch_brightness_handler,
    };
    ret = ws_manager_server_register_uri(&api_touch_brightness);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Register %s failed: %s", api_touch_brightness.uri, esp_err_to_name(ret));
    }

    const httpd_uri_t api_touch_reset = {
        .uri     = "/api/touch/reset",
        .method  = HTTP_POST,
        .handler = rest_api_touch_reset_handler,
    };
    ret = ws_manager_server_register_uri(&api_touch_reset);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Register %s failed: %s", api_touch_reset.uri, esp_err_to_name(ret));
    }

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

    /* Create action queue and dedicated task for HTTP-heavy actions */
    s_action_queue = xQueueCreate(TOUCH_ACTION_QUEUE_LEN, sizeof(touch_action_type_t));
    if (!s_action_queue) {
        ESP_LOGE(TAG, "Failed to create action queue");
        vSemaphoreDelete(s_wake_mutex);
        s_wake_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(touch_action_task, "touch_act", TOUCH_ACTION_TASK_STACK, NULL, TOUCH_ACTION_TASK_PRIO,
                    &s_action_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create action task");
        vQueueDelete(s_action_queue);
        s_action_queue = NULL;
        vSemaphoreDelete(s_wake_mutex);
        s_wake_mutex = NULL;
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
    ESP_LOGI(TAG, "Touch handler initialized (brightness default offset: +%d%%)",
             s_brightness_levels[s_brightness_level_idx]);
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

    /* Stop the action task and delete the queue */
    if (s_action_task) {
        vTaskDelete(s_action_task);
        s_action_task = NULL;
    }
    if (s_action_queue) {
        vQueueDelete(s_action_queue);
        s_action_queue = NULL;
    }

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
