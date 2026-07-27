#include "led_effects.h"

#if (LED_MODULE_ENABLE == 1)

#include "esp_log.h"

#include "led_controller.h"
#include "led_module_config.h"
#include "ws2812_module.h"

static const char* TAG = "led_effects";

#if (LED_TYPE == LED_TYPE_WS2812)
#define LED_EFFECTS_DEFAULT_BLUE ((ws2812_rgb_t){.red = 0, .green = 0, .blue = 255})
#define LED_EFFECTS_WARNING_YELLOW ((ws2812_rgb_t){.red = 255, .green = 160, .blue = 0})
#define LED_EFFECTS_ERROR_RED ((ws2812_rgb_t){.red = 255, .green = 0, .blue = 0})
#define LED_EFFECTS_RUNNING_GREEN ((ws2812_rgb_t){.red = 0, .green = 255, .blue = 32})
#define LED_EFFECTS_HEARTBEAT_RED ((ws2812_rgb_t){.red = 255, .green = 0, .blue = 0})
#define LED_EFFECTS_BREATH_GREEN                     \
    ((ws2812_rgb_t){.red   = LED_COLOR_BREATH_RED,   \
                    .green = LED_COLOR_BREATH_GREEN, \
                    .blue  = LED_COLOR_BREATH_BLUE})
#define LED_EFFECTS_SEDENTARY_ACTIVE_GREEN ((ws2812_rgb_t){.red = 0, .green = 255, .blue = 0})
#define LED_EFFECTS_SEDENTARY_MICRO_YELLOW ((ws2812_rgb_t){.red = 255, .green = 255, .blue = 0})
#define LED_EFFECTS_SEDENTARY_STATIC_BLUE ((ws2812_rgb_t){.red = 0, .green = 0, .blue = 255})
#define LED_EFFECTS_SEDENTARY_LONG_RED ((ws2812_rgb_t){.red = 255, .green = 0, .blue = 0})
#endif

#define LED_EFFECTS_BLINK_FAST_PERIOD_MS 200
#define LED_EFFECTS_BLINK_SLOW_PERIOD_MS 500

static led_blink_mode_t s_blink_mode  = LED_BLINK_MODE_NONE;
static bool             s_blink_state = false;

esp_err_t led_effects_show_breath(uint32_t elapsed_ms)
{
    if (!led_controller_is_initialized())
    {
        return ESP_ERR_INVALID_STATE;
    }

#if (LED_TYPE == LED_TYPE_WS2812)
    esp_err_t ret =
        ws2812_module_set_rgb(LED_EFFECTS_BREATH_GREEN.red, LED_EFFECTS_BREATH_GREEN.green,
                              LED_EFFECTS_BREATH_GREEN.blue);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set default breath color: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = ws2812_module_set_brightness(0xFF);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to reset breath brightness scale: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = ws2812_module_show_breath_step(elapsed_ms);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to render breath step: %s", esp_err_to_name(ret));
        return ret;
    }
#endif

    return ESP_OK;
}

esp_err_t led_effects_show_blink(led_blink_mode_t mode, uint32_t elapsed_ms)
{
    if (!led_controller_is_initialized())
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (mode == LED_BLINK_MODE_NONE)
    {
        return led_controller_turn_off();
    }

    uint32_t period_ms      = (mode == LED_BLINK_MODE_FAST) ? LED_EFFECTS_BLINK_FAST_PERIOD_MS
                                                            : LED_EFFECTS_BLINK_SLOW_PERIOD_MS;
    uint32_t half_period_ms = period_ms / 2;
    uint32_t phase_ms       = elapsed_ms % period_ms;

    bool new_state = (phase_ms < half_period_ms);

    if (new_state != s_blink_state || s_blink_mode != mode)
    {
        s_blink_state = new_state;
        s_blink_mode  = mode;

#if (LED_TYPE == LED_TYPE_WS2812)
        esp_err_t ret = ws2812_module_set_brightness(new_state ? 0xFF : 0);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to set blink brightness: %s", esp_err_to_name(ret));
            return ret;
        }
#endif
    }

    return ESP_OK;
}

esp_err_t led_effects_show_heartbeat_level(uint8_t level)
{
    if (!led_controller_is_initialized())
    {
        return ESP_ERR_INVALID_STATE;
    }

#if (LED_TYPE == LED_TYPE_WS2812)
    const uint8_t brightness =
        (uint8_t)(LED_BREATH_MIN_BRIGHTNESS + ((255U - LED_BREATH_MIN_BRIGHTNESS) * level) / 255U);

    return led_controller_set_color(LED_EFFECTS_HEARTBEAT_RED, brightness);
#else
    return ESP_OK;
#endif
}

esp_err_t led_effects_show_sedentary_state(radar_motion_state_t state)
{
    if (!led_controller_is_initialized())
    {
        return ESP_ERR_INVALID_STATE;
    }

#if (LED_TYPE == LED_TYPE_WS2812)
    ws2812_rgb_t color = LED_EFFECTS_SEDENTARY_STATIC_BLUE;

    switch (state)
    {
    case RADAR_MOTION_STATE_ACTIVE:
        color = LED_EFFECTS_SEDENTARY_ACTIVE_GREEN;
        break;
    case RADAR_MOTION_STATE_MICRO_MOTION:
        color = LED_EFFECTS_SEDENTARY_MICRO_YELLOW;
        break;
    case RADAR_MOTION_STATE_SEDENTARY:
        color = LED_EFFECTS_SEDENTARY_LONG_RED;
        break;
    case RADAR_MOTION_STATE_STATIONARY:
    case RADAR_MOTION_STATE_IDLE:
    default:
        color = LED_EFFECTS_SEDENTARY_STATIC_BLUE;
        break;
    }

    return led_controller_set_color(color, LED_BREATH_MAX_BRIGHTNESS);
#else
    return ESP_OK;
#endif
}

esp_err_t led_effects_show_system_state(led_module_system_state_t state)
{
    if (!led_controller_is_initialized())
    {
        return ESP_ERR_INVALID_STATE;
    }

    switch (state)
    {
    case LED_MODULE_SYSTEM_STATE_IDLE:
        return led_controller_turn_off();

    case LED_MODULE_SYSTEM_STATE_STARTING:
#if (LED_TYPE == LED_TYPE_WS2812)
        return led_controller_set_color(LED_EFFECTS_DEFAULT_BLUE, LED_BREATH_MAX_BRIGHTNESS);
#else
        return ESP_OK;
#endif

    case LED_MODULE_SYSTEM_STATE_RUNNING:
#if (LED_TYPE == LED_TYPE_WS2812)
        return led_controller_set_color(LED_EFFECTS_RUNNING_GREEN, LED_BREATH_MAX_BRIGHTNESS);
#else
        return ESP_OK;
#endif

    case LED_MODULE_SYSTEM_STATE_WARNING:
#if (LED_TYPE == LED_TYPE_WS2812)
        return led_controller_set_color(LED_EFFECTS_WARNING_YELLOW, LED_BREATH_MAX_BRIGHTNESS);
#else
        return ESP_OK;
#endif

    case LED_MODULE_SYSTEM_STATE_ERROR:
#if (LED_TYPE == LED_TYPE_WS2812)
        return led_controller_set_color(LED_EFFECTS_ERROR_RED, LED_BREATH_MAX_BRIGHTNESS);
#else
        return ESP_OK;
#endif

    default:
        return ESP_ERR_INVALID_ARG;
    }
}

#endif
