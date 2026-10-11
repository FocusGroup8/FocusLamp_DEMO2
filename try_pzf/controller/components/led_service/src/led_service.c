/*
 * led_service.c - Enhanced LED lighting service implementation
 *
 * Integrates:
 * - LED effects (breath, blink, heartbeat, sedentary, system state)
 * - Presence sensing (auto-off, distance-based)
 * - Light control (auto brightness via UART)
 *
 * Uses event_bus for event-driven communication and led_driver for
 * low-level WS2812 control via RMT.
 */

#include "led_service.h"

#include <string.h>
#include <math.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "led_driver.h"
#include "event_bus.h"
#include "event_def.h"
#include "led_types.h"
#include "system_config.h"   /* LED_BRIGHTNESS_MAX */
#include "device_state.h"

static const char* TAG = "led_service";

/* ===================== Internal State ===================== */

static bool s_initialized = false;
static bool s_running     = false;

/* Retained for the service lifetime so late callers can safely observe deinit. */
static SemaphoreHandle_t s_service_mutex = NULL;
static TaskHandle_t s_update_task = NULL;
static uint32_t s_elapsed = 0;
static int64_t s_last_update_error_us = -1000000;

static const event_type_t s_event_types[] = {
    EV_SENSOR_RADAR_DETECTED,
    EV_SENSOR_RADAR_CLEAR,
    EV_LIGHT_TOGGLE,
    EV_LIGHT_BRIGHTNESS_UP,
    EV_LIGHT_BRIGHTNESS_DOWN,
    EV_LIGHT_PRESET_COLOR,
};
static size_t s_subscription_count = 0;

static bool led_service_lock_initialized(void)
{
    if (s_service_mutex == NULL ||
        xSemaphoreTakeRecursive(s_service_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    if (!s_initialized) {
        xSemaphoreGiveRecursive(s_service_mutex);
        return false;
    }
    return true;
}

static esp_err_t led_service_unlock_result(esp_err_t result)
{
    xSemaphoreGiveRecursive(s_service_mutex);
    return result;
}

/* Keep a 20 ms cadence; an overrun starts a fresh period instead of busy catch-up. */
static void led_service_update_task(void *context)
{
    (void)context;
    TickType_t period_ticks = pdMS_TO_TICKS(LED_REFRESH_INTERVAL_MS);
    if (period_ticks == 0) {
        period_ticks = 1;
    }
    TickType_t last_wake = xTaskGetTickCount();
    int64_t last_update_us = esp_timer_get_time();

    for (;;) {
        TickType_t now_ticks = xTaskGetTickCount();
        if ((now_ticks - last_wake) >= period_ticks) {
            last_wake = now_ticks;
        }
        vTaskDelayUntil(&last_wake, period_ticks);

        int64_t now_us = esp_timer_get_time();
        uint32_t dt_ms = (uint32_t)((now_us - last_update_us) / 1000);
        last_update_us = now_us;
        led_service_update(dt_ms);
    }
}

/* Current mode/effect tracking */
static led_module_mode_t      s_current_mode   = LED_MODULE_MODE_OFF;
static ws2812_rgb_t           s_current_color  = {0};
static uint8_t                s_current_brightness = LED_BREATH_MAX_BRIGHTNESS;
static uint8_t                s_current_brightness_level = LED_BRIGHTNESS_LEVEL_MAX;

/* Brightness level mapping (0-5) to 0-30 (PWM max 30) */
static const uint8_t s_brightness_level_map[LED_BRIGHTNESS_LEVEL_COUNT] = {
    0,   /* level 0: off */
    6,   /* level 1: dim (~20%) */
    12,  /* level 2: low (~40%) */
    18,  /* level 3: normal (~60%) */
    24,  /* level 4: bright (~80%) */
    30,  /* level 5: max (PWM <= 30) */
};

/* 5 common color presets */
typedef struct {
    uint8_t r, g, b;
} preset_color_t;

static const preset_color_t s_preset_colors[LED_PRESET_COLOR_COUNT] = {
    { .r = 255, .g = 180, .b = 80  },  /* LED_PRESET_WARM_WHITE */
    { .r = 200, .g = 220, .b = 255 },  /* LED_PRESET_COOL_WHITE */
    { .r = 255, .g = 0,   .b = 0   },  /* LED_PRESET_RED */
    { .r = 0,   .g = 255, .b = 0   },  /* LED_PRESET_GREEN */
    { .r = 0,   .g = 0,   .b = 255 },  /* LED_PRESET_BLUE */
};

/* Blink state */
static led_blink_mode_t       s_blink_mode     = LED_BLINK_MODE_NONE;
static bool                   s_blink_state    = false;

/* Presence auto-off state machine */
typedef enum {
    PRESENCE_AUTO_OFF_IDLE,
    PRESENCE_AUTO_OFF_ACTIVE,
    PRESENCE_AUTO_OFF_FADING,
    PRESENCE_AUTO_OFF_OFF,
} presence_auto_off_state_t;

typedef enum {
    PRESENCE_DISTANCE_UNKNOWN,
    PRESENCE_DISTANCE_NEAR,
    PRESENCE_DISTANCE_FAR,
} presence_distance_state_t;

static presence_auto_off_state_t s_auto_off_state     = PRESENCE_AUTO_OFF_IDLE;
static uint32_t                  s_last_activity_time = 0;
static uint8_t                   s_fade_brightness    = 0;
static uint32_t                  s_fade_start_time    = 0;

static presence_distance_state_t s_presence_state     = PRESENCE_DISTANCE_UNKNOWN;
static float                     s_last_distance_cm   = 0.0f;

/* Presence thresholds */
#define PRESENCE_NEAR_THRESHOLD_CM  100.0f
#define PRESENCE_FAR_THRESHOLD_CM   300.0f
#define PRESENCE_FADE_DURATION_MS   2000

/* Blink periods */
#define BLINK_FAST_PERIOD_MS  200
#define BLINK_SLOW_PERIOD_MS  500

/* ===================== Event Handler ===================== */

static void led_service_event_handler(event_t* event, void* context)
{
    (void)context;
    if (!led_service_lock_initialized()) {
        return;
    }

    esp_err_t ret = ESP_OK;
    switch (event->type) {
    case EV_SENSOR_RADAR_DETECTED:
        ret = led_service_show_presence(true);
        if (ret == ESP_OK) {
            ret = led_service_trigger_activity();
        }
        break;
    case EV_SENSOR_RADAR_CLEAR:
        ret = led_service_show_presence(false);
        break;
    case EV_LIGHT_TOGGLE:
        ret = (s_current_mode != LED_MODULE_MODE_OFF)
            ? led_service_turn_off()
            : led_service_turn_on(s_current_brightness);
        break;
    case EV_LIGHT_BRIGHTNESS_UP:
        ret = led_service_brightness_up();
        break;
    case EV_LIGHT_BRIGHTNESS_DOWN:
        ret = led_service_brightness_down();
        break;
    case EV_LIGHT_PRESET_COLOR:
        if (event->data != NULL && event->data_size == sizeof(uint8_t)) {
            uint8_t preset = *(uint8_t*)event->data;
            ret = led_service_set_preset_color((led_preset_color_t)preset);
        }
        break;
    default:
        break;
    }

    xSemaphoreGiveRecursive(s_service_mutex);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "LED event 0x%04X failed: %s", event->type, esp_err_to_name(ret));
    }
}

/* ===================== LED Effects (Internal) ===================== */

static esp_err_t led_service_sync_device_state(void)
{
    bool on = (s_current_mode != LED_MODULE_MODE_OFF);
    return device_state_set_light(s_current_brightness_level, s_current_brightness,
                                  s_current_color.red, s_current_color.green,
                                  s_current_color.blue, on);
}

static esp_err_t led_service_set_solid_color(ws2812_rgb_t color, uint8_t brightness)
{
    if (!led_driver_is_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = led_driver_set_all(color.red, color.green, color.blue);
    if (ret != ESP_OK) return ret;

    if (brightness > LED_BRIGHTNESS_MAX) {
        brightness = LED_BRIGHTNESS_MAX;
    }
    led_driver_set_brightness(brightness);
    return led_driver_show();
}

/* Convert HSV (hue 0-359, saturation/value 0-255) to RGB. */
static ws2812_rgb_t led_service_hsv_to_rgb(uint16_t hue, uint8_t saturation, uint8_t value)
{
    hue %= 360;
    uint8_t sector = (uint8_t)(hue / 60);
    uint8_t fraction = (uint8_t)(((hue % 60) * 255U) / 60U);
    uint8_t p = (uint8_t)(((uint16_t)value * (255U - saturation)) / 255U);
    uint8_t q = (uint8_t)(((uint16_t)value *
        (255U - ((uint16_t)saturation * fraction) / 255U)) / 255U);
    uint8_t t = (uint8_t)(((uint16_t)value *
        (255U - ((uint16_t)saturation * (255U - fraction)) / 255U)) / 255U);

    switch (sector) {
    case 0: return (ws2812_rgb_t){.red = value, .green = t, .blue = p};
    case 1: return (ws2812_rgb_t){.red = q, .green = value, .blue = p};
    case 2: return (ws2812_rgb_t){.red = p, .green = value, .blue = t};
    case 3: return (ws2812_rgb_t){.red = p, .green = q, .blue = value};
    case 4: return (ws2812_rgb_t){.red = t, .green = p, .blue = value};
    default: return (ws2812_rgb_t){.red = value, .green = p, .blue = q};
    }
}

/* Only valid LEDs are written; the driver's physical tail stays zero-filled. */
static esp_err_t led_service_show_rainbow(uint32_t elapsed_ms)
{
    const uint32_t period_ms = 6000;
    uint16_t phase = (uint16_t)(((elapsed_ms % period_ms) * 360U) / period_ms);

    for (uint8_t i = 0; i < LED_NUM_LEDS; ++i) {
        uint16_t hue = (uint16_t)((phase + ((uint32_t)i * 360U) / LED_NUM_LEDS) % 360U);
        ws2812_rgb_t color = led_service_hsv_to_rgb(hue, 255, 255);
        esp_err_t ret = led_driver_set_color(i, color.red, color.green, color.blue);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    led_driver_set_brightness(s_current_brightness);
    return led_driver_show();
}

static void led_service_track_brightness(uint8_t brightness)
{
    s_current_brightness = brightness > LED_BRIGHTNESS_MAX ? LED_BRIGHTNESS_MAX : brightness;
    s_current_brightness_level = LED_BRIGHTNESS_LEVEL_MAX;
    for (int i = LED_BRIGHTNESS_LEVEL_MAX; i >= 0; --i) {
        if (s_current_brightness >= s_brightness_level_map[i]) {
            s_current_brightness_level = (uint8_t)i;
            break;
        }
    }
}

static void led_service_unsubscribe_events(void)
{
    while (s_subscription_count > 0) {
        --s_subscription_count;
        int result = event_bus_unsubscribe(s_event_types[s_subscription_count],
                                           led_service_event_handler);
        if (result != 0) {
            ESP_LOGW(TAG, "LED event unsubscribe failed: %d", result);
        }
    }
}

/* ===================== Public API ===================== */

esp_err_t led_service_init(void)
{
    /* app_main initializes the service before exposing its command endpoints. */
    if (s_service_mutex == NULL) {
        s_service_mutex = xSemaphoreCreateRecursiveMutex();
        if (s_service_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (xSemaphoreTakeRecursive(s_service_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_initialized) {
        return led_service_unlock_result(ESP_OK);
    }

    esp_err_t ret = led_driver_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init LED driver: %s", esp_err_to_name(ret));
        return led_service_unlock_result(ret);
    }

    s_current_mode = LED_MODULE_MODE_SOLID;
    s_current_color = (ws2812_rgb_t){.red = 255, .green = 255, .blue = 255};
    s_current_brightness_level = 3;
    s_current_brightness = s_brightness_level_map[s_current_brightness_level];
    s_elapsed = 0;
    s_last_update_error_us = -1000000;
    s_blink_mode = LED_BLINK_MODE_NONE;
    s_blink_state = false;
    s_auto_off_state = PRESENCE_AUTO_OFF_IDLE;
    s_presence_state = PRESENCE_DISTANCE_UNKNOWN;

    ret = led_service_set_solid_color(s_current_color, s_current_brightness);
    if (ret == ESP_OK) {
        ret = led_service_sync_device_state();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set default LED state: %s", esp_err_to_name(ret));
        goto fail;
    }

    for (size_t i = 0; i < sizeof(s_event_types) / sizeof(s_event_types[0]); ++i) {
        int result = event_bus_subscribe(s_event_types[i], led_service_event_handler, NULL);
        if (result != 0) {
            ESP_LOGE(TAG, "Failed to subscribe LED event 0x%04X: %d", s_event_types[i], result);
            ret = (result == -6) ? ESP_ERR_NO_MEM
                : (result == -4) ? ESP_ERR_INVALID_STATE : ESP_FAIL;
            goto fail;
        }
        ++s_subscription_count;
    }

    s_initialized = true;
    s_running = true;
    ESP_LOGI(TAG, "LED service initialized");
    return led_service_unlock_result(ESP_OK);

fail:
    led_service_unsubscribe_events();
    (void)led_driver_clear();
    (void)led_driver_deinit();
    s_current_mode = LED_MODULE_MODE_OFF;
    s_running = false;
    (void)led_service_sync_device_state();
    return led_service_unlock_result(ret);
}

esp_err_t led_service_start(void)
{
    if (!led_service_lock_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_update_task != NULL) {
        return led_service_unlock_result(ESP_OK);
    }

    BaseType_t result = xTaskCreatePinnedToCore(led_service_update_task, "led_service",
                                              4096, NULL, 2, &s_update_task,
                                              tskNO_AFFINITY);
    if (result != pdPASS) {
        s_update_task = NULL;
        return led_service_unlock_result(ESP_ERR_NO_MEM);
    }
    ESP_LOGI(TAG, "LED update task started (period=%d ms, stack=4096, priority=2)",
             LED_REFRESH_INTERVAL_MS);
    return led_service_unlock_result(ESP_OK);
}

esp_err_t led_service_deinit(void)
{
    if (s_service_mutex == NULL) {
        return ESP_OK;
    }
    if (xSemaphoreTakeRecursive(s_service_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_initialized && !led_driver_is_initialized()) {
        return led_service_unlock_result(ESP_OK);
    }

    s_running = false;
    s_initialized = false;
    /* We own the transaction mutex, so the task cannot be inside driver I/O. */
    if (s_update_task != NULL) {
        vTaskDelete(s_update_task);
        s_update_task = NULL;
    }
    led_service_unsubscribe_events();
    esp_err_t ret = led_driver_deinit();
    s_current_mode = LED_MODULE_MODE_OFF;
    (void)led_service_sync_device_state();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "LED service deinitialized");
    } else {
        ESP_LOGW(TAG, "LED driver cleanup failed: %s", esp_err_to_name(ret));
    }
    return led_service_unlock_result(ret);
}

esp_err_t led_service_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    if (!led_service_lock_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }


    s_current_color = (ws2812_rgb_t){.red = r, .green = g, .blue = b};
    s_current_mode  = LED_MODULE_MODE_SOLID;

    esp_err_t ret = led_service_set_solid_color(s_current_color, s_current_brightness);
    esp_err_t state_ret = led_service_sync_device_state();
    return led_service_unlock_result(ret != ESP_OK ? ret : state_ret);
}

esp_err_t led_service_set_brightness(uint8_t brightness)
{
    if (!led_service_lock_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    led_service_track_brightness(brightness);
    led_driver_set_brightness(s_current_brightness);

    esp_err_t ret = ESP_OK;
    if (s_current_mode == LED_MODULE_MODE_SOLID) {
        ret = led_driver_show();
    } else if (s_current_mode == LED_MODULE_MODE_RAINBOW) {
        ret = led_service_show_rainbow(s_elapsed);
    }
    esp_err_t state_ret = led_service_sync_device_state();
    return led_service_unlock_result(ret != ESP_OK ? ret : state_ret);
}

esp_err_t led_service_set_brightness_level(uint8_t level)
{
    if (!led_service_lock_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (level > LED_BRIGHTNESS_LEVEL_MAX) {
        level = LED_BRIGHTNESS_LEVEL_MAX;
    }

    s_current_brightness_level = level;
    s_current_brightness = s_brightness_level_map[level];

    if (level == 0) {
        return led_service_unlock_result(led_service_turn_off());
    }

    return led_service_unlock_result(led_service_set_brightness(s_current_brightness));
}

uint8_t led_service_level_from_ambient(uint8_t ambient_level)
{
    /* 光感档位(0-4) → 亮度档位(1-5)，同向一一对应：暗→低档、亮→高档 */
    if (ambient_level >= (uint8_t)(LED_BRIGHTNESS_LEVEL_MAX - 1)) {
        return LED_BRIGHTNESS_LEVEL_MAX;
    }
    return (uint8_t)(ambient_level + 1);
}

uint8_t led_service_get_brightness_level(void)
{
    if (!led_service_lock_initialized()) {
        return 0;
    }
    uint8_t level = s_current_brightness_level;
    xSemaphoreGiveRecursive(s_service_mutex);
    return level;
}

esp_err_t led_service_brightness_up(void)
{
    if (!led_service_lock_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_current_brightness_level >= LED_BRIGHTNESS_LEVEL_MAX) {
        return led_service_unlock_result(ESP_OK); /* already max */
    }
    return led_service_unlock_result(led_service_set_brightness_level(s_current_brightness_level + 1));
}

esp_err_t led_service_brightness_down(void)
{
    if (!led_service_lock_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_current_brightness_level == 0) {
        return led_service_unlock_result(ESP_OK); /* already off */
    }
    return led_service_unlock_result(led_service_set_brightness_level(s_current_brightness_level - 1));
}

esp_err_t led_service_set_effect(led_effect_t effect)
{
    if (!led_service_lock_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret;
    switch (effect) {
    case LED_EFFECT_STEADY:
        s_current_mode = LED_MODULE_MODE_SOLID;
        ret = led_service_set_solid_color(s_current_color, s_current_brightness);
        break;
    case LED_EFFECT_BREATHING:
        s_current_mode = LED_MODULE_MODE_BREATH;
        ret = led_service_show_breath(s_elapsed);
        break;
    case LED_EFFECT_BLINKING:
        s_current_mode = LED_MODULE_MODE_BLINK;
        ret = led_driver_set_all(s_current_color.red, s_current_color.green,
                                 s_current_color.blue);
        if (ret == ESP_OK) {
            s_blink_mode = LED_BLINK_MODE_NONE;
            ret = led_service_show_blink(LED_BLINK_MODE_SLOW, s_elapsed);
        }
        break;
    case LED_EFFECT_RAINBOW:
        s_current_mode = LED_MODULE_MODE_RAINBOW;
        ret = led_service_show_rainbow(s_elapsed);
        break;
    case LED_EFFECT_MUSIC_RHYTHM:
        return led_service_unlock_result(ESP_ERR_NOT_SUPPORTED);
    default:
        return led_service_unlock_result(ESP_ERR_INVALID_ARG);
    }

    esp_err_t state_ret = led_service_sync_device_state();
    return led_service_unlock_result(ret != ESP_OK ? ret : state_ret);
}

esp_err_t led_service_set_mode(led_mode_t mode)
{
    if (!led_service_lock_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    (void)mode;
    /* Mode mapping not yet implemented */
    return led_service_unlock_result(ESP_OK);
}

esp_err_t led_service_set_preset_color(led_preset_color_t preset)
{
    if (!led_service_lock_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (preset >= LED_PRESET_COLOR_COUNT) {
        return led_service_unlock_result(ESP_ERR_INVALID_ARG);
    }

    const preset_color_t* pc = &s_preset_colors[preset];
    ESP_LOGI(TAG, "Set preset color #%d (R=%d, G=%d, B=%d)", preset, pc->r, pc->g, pc->b);

    return led_service_unlock_result(led_service_set_color(pc->r, pc->g, pc->b));
}

void led_service_update(uint32_t dt_ms)
{
    if (!led_service_lock_initialized()) {
        return;
    }
    if (!s_running) {
        xSemaphoreGiveRecursive(s_service_mutex);
        return;
    }

    s_elapsed += dt_ms;
    esp_err_t ret = ESP_OK;
    switch (s_current_mode) {
    case LED_MODULE_MODE_BREATH:
        ret = led_service_show_breath(s_elapsed);
        break;
    case LED_MODULE_MODE_BLINK:
        ret = led_service_show_blink(s_blink_mode, s_elapsed);
        break;
    case LED_MODULE_MODE_RAINBOW:
        ret = led_service_show_rainbow(s_elapsed);
        break;
    case LED_MODULE_MODE_PRESENCE:
        ret = led_service_update_auto_off(event_bus_get_timestamp());
        break;
    default:
        break;
    }

    bool log_error = false;
    if (ret != ESP_OK) {
        int64_t now_us = esp_timer_get_time();
        if ((now_us - s_last_update_error_us) >= 1000000) {
            s_last_update_error_us = now_us;
            log_error = true;
        }
    } else {
        s_last_update_error_us = -1000000;
    }
    xSemaphoreGiveRecursive(s_service_mutex);
    if (log_error) {
        ESP_LOGW(TAG, "LED animation update failed: %s", esp_err_to_name(ret));
    }
}

esp_err_t led_service_turn_off(void)
{
    if (!led_service_lock_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }


    s_current_mode = LED_MODULE_MODE_OFF;
    esp_err_t ret = led_driver_clear();
    esp_err_t state_ret = led_service_sync_device_state();
    return led_service_unlock_result(ret != ESP_OK ? ret : state_ret);
}

esp_err_t led_service_turn_on(uint8_t raw_brightness)
{
    if (!led_service_lock_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    led_service_track_brightness(raw_brightness);
    s_current_mode = LED_MODULE_MODE_SOLID;
    esp_err_t ret = led_service_set_solid_color(s_current_color, s_current_brightness);
    esp_err_t state_ret = led_service_sync_device_state();
    return led_service_unlock_result(ret != ESP_OK ? ret : state_ret);
}

/* ===================== LED Effects ===================== */

esp_err_t led_service_show_breath(uint32_t elapsed_ms)
{
    if (!led_service_lock_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }


    uint32_t period_ms = LED_BREATH_DEFAULT_PERIOD_MS;
    uint32_t phase_ms = elapsed_ms % period_ms;

    /* Sine-wave breathing: brightness oscillates between min and max */
    float phase = (float)phase_ms / (float)period_ms * 2.0f * 3.14159f;
    float sine_val = (sinf(phase) + 1.0f) / 2.0f;  /* 0.0 ~ 1.0 */

    uint8_t breath_brightness = (uint8_t)(LED_BREATH_MIN_BRIGHTNESS +
        (uint16_t)((LED_BREATH_MAX_BRIGHTNESS - LED_BREATH_MIN_BRIGHTNESS) * sine_val));

    esp_err_t ret = led_driver_set_all(
        LED_COLOR_BREATH_RED, LED_COLOR_BREATH_GREEN, LED_COLOR_BREATH_BLUE);
    if (ret != ESP_OK) return led_service_unlock_result(ret);

    led_driver_set_brightness(breath_brightness);
    return led_service_unlock_result(led_driver_show());
}

esp_err_t led_service_show_blink(led_blink_mode_t mode, uint32_t elapsed_ms)
{
    if (!led_service_lock_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }


    if (mode == LED_BLINK_MODE_NONE) {
        return led_service_unlock_result(led_driver_clear());
    }

    uint32_t period_ms = (mode == LED_BLINK_MODE_FAST) ? BLINK_FAST_PERIOD_MS
                                                        : BLINK_SLOW_PERIOD_MS;
    uint32_t half_period_ms = period_ms / 2;
    uint32_t phase_ms       = elapsed_ms % period_ms;
    bool     new_state      = (phase_ms < half_period_ms);

    if (new_state != s_blink_state || s_blink_mode != mode) {
        s_blink_state = new_state;
        s_blink_mode  = mode;

        led_driver_set_brightness(new_state ? s_current_brightness : 0);
        return led_service_unlock_result(led_driver_show());
    }

    return led_service_unlock_result(ESP_OK);
}

esp_err_t led_service_show_heartbeat_level(uint8_t level)
{
    if (!led_service_lock_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }


    ws2812_rgb_t heartbeat_color = {.red = 255, .green = 0, .blue = 0};
    uint8_t brightness = (uint8_t)(LED_BREATH_MIN_BRIGHTNESS +
        ((LED_BRIGHTNESS_MAX - LED_BREATH_MIN_BRIGHTNESS) * level) / 255U);

    return led_service_unlock_result(led_service_set_solid_color(heartbeat_color, brightness));
}

esp_err_t led_service_show_sedentary_state(radar_motion_state_t state)
{
    if (!led_service_lock_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }


    ws2812_rgb_t color;

    switch (state) {
    case RADAR_MOTION_STATE_ACTIVE:
        color = (ws2812_rgb_t){.red = 0, .green = 255, .blue = 0};
        break;
    case RADAR_MOTION_STATE_MICRO_MOTION:
        color = (ws2812_rgb_t){.red = 255, .green = 255, .blue = 0};
        break;
    case RADAR_MOTION_STATE_SEDENTARY:
        color = (ws2812_rgb_t){.red = 255, .green = 0, .blue = 0};
        break;
    case RADAR_MOTION_STATE_STATIONARY:
    case RADAR_MOTION_STATE_IDLE:
    default:
        color = (ws2812_rgb_t){.red = 0, .green = 0, .blue = 255};
        break;
    }

    return led_service_unlock_result(led_service_set_solid_color(color, LED_BREATH_MAX_BRIGHTNESS));
}

esp_err_t led_service_show_system_state(led_module_system_state_t state)
{
    if (!led_service_lock_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }


    switch (state) {
    case LED_MODULE_SYSTEM_STATE_IDLE:
        return led_service_unlock_result(led_driver_clear());

    case LED_MODULE_SYSTEM_STATE_STARTING:
        return led_service_unlock_result(led_service_set_solid_color(
            (ws2812_rgb_t){.red = 0, .green = 0, .blue = 255}, LED_BREATH_MAX_BRIGHTNESS));

    case LED_MODULE_SYSTEM_STATE_RUNNING:
        return led_service_unlock_result(led_service_set_solid_color(
            (ws2812_rgb_t){.red = 0, .green = 255, .blue = 32}, LED_BREATH_MAX_BRIGHTNESS));

    case LED_MODULE_SYSTEM_STATE_WARNING:
        return led_service_unlock_result(led_service_set_solid_color(
            (ws2812_rgb_t){.red = 255, .green = 160, .blue = 0}, LED_BREATH_MAX_BRIGHTNESS));

    case LED_MODULE_SYSTEM_STATE_ERROR:
        return led_service_unlock_result(led_service_set_solid_color(
            (ws2812_rgb_t){.red = 255, .green = 0, .blue = 0}, LED_BREATH_MAX_BRIGHTNESS));

    default:
        return led_service_unlock_result(ESP_ERR_INVALID_ARG);
    }
}

/* ===================== Presence Sensing ===================== */

esp_err_t led_service_show_presence(bool is_human_present)
{
    if (!led_service_lock_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }


    if (!is_human_present) {
        return led_service_unlock_result(led_driver_clear());
    }

    s_current_mode = LED_MODULE_MODE_PRESENCE;
    return led_service_unlock_result(led_service_set_solid_color(
        (ws2812_rgb_t){.red = 0, .green = 0, .blue = 255}, LED_BREATH_MAX_BRIGHTNESS));
}

esp_err_t led_service_update_presence_distance(float distance_cm)
{
    if (!led_service_lock_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }


    s_last_distance_cm = distance_cm;

    presence_distance_state_t new_state = s_presence_state;

    if (distance_cm < PRESENCE_NEAR_THRESHOLD_CM) {
        new_state = PRESENCE_DISTANCE_NEAR;
    } else if (distance_cm > PRESENCE_FAR_THRESHOLD_CM) {
        new_state = PRESENCE_DISTANCE_FAR;
    }

    if (new_state != s_presence_state) {
        s_presence_state = new_state;

        if (new_state == PRESENCE_DISTANCE_NEAR) {
            ESP_LOGI(TAG, "Presence: NEAR (%.1f cm)", distance_cm);
            (void)led_service_trigger_activity();
        } else if (new_state == PRESENCE_DISTANCE_FAR) {
            ESP_LOGI(TAG, "Presence: FAR (%.1f cm)", distance_cm);
        }
    }

    return led_service_unlock_result(ESP_OK);
}

esp_err_t led_service_trigger_activity(void)
{
    if (!led_service_lock_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }


    s_last_activity_time = esp_timer_get_time() / 1000;
    s_auto_off_state     = PRESENCE_AUTO_OFF_ACTIVE;

    return led_service_unlock_result(ESP_OK);
}

esp_err_t led_service_update_auto_off(uint32_t current_time_ms)
{
    if (!led_service_lock_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }


    led_module_snapshot_t snapshot = {0};
    esp_err_t             ret      = led_driver_get_snapshot(&snapshot);
    if (ret != ESP_OK) {
        return led_service_unlock_result(ret);
    }

    switch (s_auto_off_state) {
    case PRESENCE_AUTO_OFF_IDLE:
        break;

    case PRESENCE_AUTO_OFF_ACTIVE:
        if ((current_time_ms - s_last_activity_time) >= LED_AUTO_OFF_TIMEOUT_MS) {
            s_auto_off_state  = PRESENCE_AUTO_OFF_FADING;
            s_fade_start_time = current_time_ms;
            s_fade_brightness = snapshot.brightness;
            ESP_LOGD(TAG, "Auto-off timeout, starting fade");
        }
        break;

    case PRESENCE_AUTO_OFF_FADING:
    {
        uint32_t elapsed = current_time_ms - s_fade_start_time;
        if (elapsed >= PRESENCE_FADE_DURATION_MS) {
            ret = led_driver_clear();
            if (ret != ESP_OK) return led_service_unlock_result(ret);
            s_auto_off_state = PRESENCE_AUTO_OFF_OFF;
            ESP_LOGI(TAG, "LED auto-off completed");
        } else {
            float   fade_progress  = (float)elapsed / PRESENCE_FADE_DURATION_MS;
            uint8_t new_brightness = (uint8_t)(s_fade_brightness * (1.0f - fade_progress));

            if (new_brightness != snapshot.brightness) {
                led_driver_set_brightness(new_brightness);
                ret = led_driver_show();
                if (ret != ESP_OK) {
                    return led_service_unlock_result(ret);
                }
            }
        }
    }
    break;

    case PRESENCE_AUTO_OFF_OFF:
        break;

    default:
        ESP_LOGW(TAG, "Unknown auto-off state: %d", s_auto_off_state);
        s_auto_off_state = PRESENCE_AUTO_OFF_IDLE;
        break;
    }

    return led_service_unlock_result(ESP_OK);
}

/* ===================== Snapshot ===================== */

esp_err_t led_service_get_snapshot(led_module_snapshot_t* snapshot)
{
    if (!led_service_lock_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (snapshot == NULL) {
        return led_service_unlock_result(ESP_ERR_INVALID_ARG);
    }

    esp_err_t ret = led_driver_get_snapshot(snapshot);
    if (ret == ESP_OK) {
        snapshot->mode             = s_current_mode;
        snapshot->brightness_level = s_current_brightness_level;
    }

    return led_service_unlock_result(ret);
}
