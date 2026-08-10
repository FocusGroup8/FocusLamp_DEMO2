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

#include "led_driver.h"
#include "event_bus.h"
#include "event_def.h"
#include "led_types.h"
#include "device_state.h"

static const char* TAG = "led_service";

/* ===================== Internal State ===================== */

static bool s_initialized = false;
static bool s_running     = false;

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
    if (!s_initialized) return;

    switch (event->type) {
    case EV_SENSOR_RADAR_DETECTED:
        /* Radar detected presence */
        led_service_show_presence(true);
        led_service_trigger_activity();
        break;

    case EV_SENSOR_RADAR_CLEAR:
        /* Radar no presence */
        led_service_show_presence(false);
        break;

    case EV_LIGHT_TOGGLE:
        if (s_current_mode != LED_MODULE_MODE_OFF) {
            led_service_turn_off();
        } else {
            led_service_set_color(s_current_color.red, s_current_color.green, s_current_color.blue);
            led_service_set_brightness(s_current_brightness);
        }
        break;

    case EV_LIGHT_BRIGHTNESS_UP:
        led_service_brightness_up();
        break;

    case EV_LIGHT_BRIGHTNESS_DOWN:
        led_service_brightness_down();
        break;

    case EV_LIGHT_PRESET_COLOR:
        if (event->data != NULL && event->data_size == sizeof(uint8_t)) {
            uint8_t preset = *(uint8_t*)event->data;
            led_service_set_preset_color((led_preset_color_t)preset);
        }
        break;

    default:
        break;
    }
}

/* ===================== LED Effects (Internal) ===================== */

static void led_service_sync_device_state(void)
{
    bool on = (s_current_mode != LED_MODULE_MODE_OFF);
    device_state_set_light(s_current_brightness_level, s_current_brightness,
                           s_current_color.red, s_current_color.green, s_current_color.blue, on);
}

static esp_err_t led_service_set_solid_color(ws2812_rgb_t color, uint8_t brightness)
{
    if (!led_driver_is_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = led_driver_set_all(color.red, color.green, color.blue);
    if (ret != ESP_OK) return ret;

    led_driver_set_brightness(brightness);
    return led_driver_show();
}

/* ===================== Public API ===================== */

esp_err_t led_service_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "LED service already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing LED service...");

    /* Initialize LED driver */
    esp_err_t ret = led_driver_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init LED driver: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Set default state */
    s_current_mode             = LED_MODULE_MODE_SOLID;
    s_current_color            = (ws2812_rgb_t){.red = 0, .green = 0, .blue = 255};
    s_current_brightness_level = 3;
    s_current_brightness       = s_brightness_level_map[s_current_brightness_level];

    /* Set initial color */
    ret = led_service_set_solid_color(s_current_color, s_current_brightness);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set default color: %s", esp_err_to_name(ret));
    }

    led_service_sync_device_state();

    /* Subscribe to events */
    event_bus_subscribe(EV_SENSOR_RADAR_DETECTED, led_service_event_handler, NULL);
    event_bus_subscribe(EV_SENSOR_RADAR_CLEAR, led_service_event_handler, NULL);
    event_bus_subscribe(EV_LIGHT_TOGGLE, led_service_event_handler, NULL);
    event_bus_subscribe(EV_LIGHT_BRIGHTNESS_UP, led_service_event_handler, NULL);
    event_bus_subscribe(EV_LIGHT_BRIGHTNESS_DOWN, led_service_event_handler, NULL);
    event_bus_subscribe(EV_LIGHT_PRESET_COLOR, led_service_event_handler, NULL);

    s_initialized = true;
    s_running     = true;

    ESP_LOGI(TAG, "LED service initialized");
    return ESP_OK;
}

esp_err_t led_service_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    s_running     = false;
    s_initialized = false;

    esp_err_t ret = led_driver_deinit();
    ESP_LOGI(TAG, "LED service deinitialized");
    return ret;
}

esp_err_t led_service_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_current_color = (ws2812_rgb_t){.red = r, .green = g, .blue = b};
    s_current_mode  = LED_MODULE_MODE_SOLID;

    esp_err_t ret = led_service_set_solid_color(s_current_color, s_current_brightness);
    led_service_sync_device_state();
    return ret;
}

esp_err_t led_service_set_brightness(uint8_t brightness)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_current_brightness = brightness;
    led_driver_set_brightness(brightness);

    /* Find closest level for tracking */
    s_current_brightness_level = LED_BRIGHTNESS_LEVEL_MAX;
    for (int i = LED_BRIGHTNESS_LEVEL_MAX; i >= 0; i--) {
        if (brightness >= s_brightness_level_map[i]) {
            s_current_brightness_level = (uint8_t)i;
            break;
        }
    }

    /* If in solid mode, re-apply to update brightness */
    if (s_current_mode == LED_MODULE_MODE_SOLID) {
        led_driver_show();
    }

    led_service_sync_device_state();
    return ESP_OK;
}

esp_err_t led_service_set_brightness_level(uint8_t level)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (level > LED_BRIGHTNESS_LEVEL_MAX) {
        level = LED_BRIGHTNESS_LEVEL_MAX;
    }

    s_current_brightness_level = level;
    s_current_brightness = s_brightness_level_map[level];

    if (level == 0) {
        return led_service_turn_off();
    }

    return led_service_set_brightness(s_current_brightness);
}

uint8_t led_service_get_brightness_level(void)
{
    return s_current_brightness_level;
}

esp_err_t led_service_brightness_up(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_current_brightness_level >= LED_BRIGHTNESS_LEVEL_MAX) {
        return ESP_OK; /* already max */
    }
    return led_service_set_brightness_level(s_current_brightness_level + 1);
}

esp_err_t led_service_brightness_down(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_current_brightness_level == 0) {
        return ESP_OK; /* already off */
    }
    return led_service_set_brightness_level(s_current_brightness_level - 1);
}

esp_err_t led_service_set_effect(led_effect_t effect)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    switch (effect) {
    case LED_EFFECT_STEADY:
        s_current_mode = LED_MODULE_MODE_SOLID;
        return led_service_set_solid_color(s_current_color, s_current_brightness);

    case LED_EFFECT_BREATHING:
        s_current_mode = LED_MODULE_MODE_BREATH;
        return ESP_OK;

    case LED_EFFECT_BLINKING:
        s_current_mode = LED_MODULE_MODE_BLINK;
        s_blink_mode   = LED_BLINK_MODE_SLOW;
        return ESP_OK;

    case LED_EFFECT_RAINBOW:
    case LED_EFFECT_MUSIC_RHYTHM:
        /* Not implemented in basic service */
        return ESP_ERR_NOT_SUPPORTED;

    default:
        return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t led_service_set_mode(led_mode_t mode)
{
    (void)mode;
    /* Mode mapping not yet implemented */
    return ESP_OK;
}

esp_err_t led_service_set_preset_color(led_preset_color_t preset)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (preset >= LED_PRESET_COLOR_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    const preset_color_t* pc = &s_preset_colors[preset];
    ESP_LOGI(TAG, "Set preset color #%d (R=%d, G=%d, B=%d)", preset, pc->r, pc->g, pc->b);

    return led_service_set_color(pc->r, pc->g, pc->b);
}

void led_service_update(uint32_t dt_ms)
{
    if (!s_initialized || !s_running) {
        return;
    }

    static uint32_t s_elapsed = 0;
    s_elapsed += dt_ms;

    switch (s_current_mode) {
    case LED_MODULE_MODE_BREATH:
        led_service_show_breath(s_elapsed);
        break;

    case LED_MODULE_MODE_BLINK:
        led_service_show_blink(s_blink_mode, s_elapsed);
        break;

    case LED_MODULE_MODE_PRESENCE:
        /* Update auto-off state machine */
        led_service_update_auto_off(event_bus_get_timestamp());
        break;

    default:
        break;
    }
}

esp_err_t led_service_turn_off(void)
{
    if (!led_driver_is_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    s_current_mode = LED_MODULE_MODE_OFF;
    esp_err_t ret = led_driver_clear();
    led_service_sync_device_state();
    return ret;
}

/* ===================== LED Effects ===================== */

esp_err_t led_service_show_breath(uint32_t elapsed_ms)
{
    if (!led_driver_is_initialized()) {
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
    if (ret != ESP_OK) return ret;

    led_driver_set_brightness(breath_brightness);
    return led_driver_show();
}

esp_err_t led_service_show_blink(led_blink_mode_t mode, uint32_t elapsed_ms)
{
    if (!led_driver_is_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (mode == LED_BLINK_MODE_NONE) {
        return led_driver_clear();
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
        return led_driver_show();
    }

    return ESP_OK;
}

esp_err_t led_service_show_heartbeat_level(uint8_t level)
{
    if (!led_driver_is_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    ws2812_rgb_t heartbeat_color = {.red = 255, .green = 0, .blue = 0};
    uint8_t brightness = (uint8_t)(LED_BREATH_MIN_BRIGHTNESS +
        ((255U - LED_BREATH_MIN_BRIGHTNESS) * level) / 255U);

    return led_service_set_solid_color(heartbeat_color, brightness);
}

esp_err_t led_service_show_sedentary_state(radar_motion_state_t state)
{
    if (!led_driver_is_initialized()) {
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

    return led_service_set_solid_color(color, LED_BREATH_MAX_BRIGHTNESS);
}

esp_err_t led_service_show_system_state(led_module_system_state_t state)
{
    if (!led_driver_is_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    switch (state) {
    case LED_MODULE_SYSTEM_STATE_IDLE:
        return led_driver_clear();

    case LED_MODULE_SYSTEM_STATE_STARTING:
        return led_service_set_solid_color(
            (ws2812_rgb_t){.red = 0, .green = 0, .blue = 255}, LED_BREATH_MAX_BRIGHTNESS);

    case LED_MODULE_SYSTEM_STATE_RUNNING:
        return led_service_set_solid_color(
            (ws2812_rgb_t){.red = 0, .green = 255, .blue = 32}, LED_BREATH_MAX_BRIGHTNESS);

    case LED_MODULE_SYSTEM_STATE_WARNING:
        return led_service_set_solid_color(
            (ws2812_rgb_t){.red = 255, .green = 160, .blue = 0}, LED_BREATH_MAX_BRIGHTNESS);

    case LED_MODULE_SYSTEM_STATE_ERROR:
        return led_service_set_solid_color(
            (ws2812_rgb_t){.red = 255, .green = 0, .blue = 0}, LED_BREATH_MAX_BRIGHTNESS);

    default:
        return ESP_ERR_INVALID_ARG;
    }
}

/* ===================== Presence Sensing ===================== */

esp_err_t led_service_show_presence(bool is_human_present)
{
    if (!led_driver_is_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!is_human_present) {
        return led_driver_clear();
    }

    s_current_mode = LED_MODULE_MODE_PRESENCE;
    return led_service_set_solid_color(
        (ws2812_rgb_t){.red = 0, .green = 0, .blue = 255}, LED_BREATH_MAX_BRIGHTNESS);
}

esp_err_t led_service_update_presence_distance(float distance_cm)
{
    if (!led_driver_is_initialized()) {
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

    return ESP_OK;
}

esp_err_t led_service_trigger_activity(void)
{
    if (!led_driver_is_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    s_last_activity_time = esp_timer_get_time() / 1000;
    s_auto_off_state     = PRESENCE_AUTO_OFF_ACTIVE;

    return ESP_OK;
}

esp_err_t led_service_update_auto_off(uint32_t current_time_ms)
{
    if (!led_driver_is_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    led_module_snapshot_t snapshot = {0};
    esp_err_t             ret      = led_driver_get_snapshot(&snapshot);
    if (ret != ESP_OK) {
        return ret;
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
            (void)led_driver_clear();
            s_auto_off_state = PRESENCE_AUTO_OFF_OFF;
            ESP_LOGI(TAG, "LED auto-off completed");
        } else {
            float   fade_progress  = (float)elapsed / PRESENCE_FADE_DURATION_MS;
            uint8_t new_brightness = (uint8_t)(s_fade_brightness * (1.0f - fade_progress));

            if (new_brightness != snapshot.brightness) {
                led_driver_set_brightness(new_brightness);
                ret = led_driver_show();
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to set fade brightness: %s", esp_err_to_name(ret));
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

    return ESP_OK;
}

/* ===================== Snapshot ===================== */

esp_err_t led_service_get_snapshot(led_module_snapshot_t* snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = led_driver_get_snapshot(snapshot);
    if (ret == ESP_OK) {
        snapshot->mode             = s_current_mode;
        snapshot->brightness_level = s_current_brightness_level;
    }

    return ret;
}
