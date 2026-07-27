#include "led_presence.h"

#if (LED_MODULE_ENABLE == 1)

#include "esp_log.h"
#include "esp_timer.h"

#include "led_controller.h"
#include "led_module_config.h"
#include "ws2812_module.h"

static const char* TAG = "led_presence";

#if (LED_TYPE == LED_TYPE_WS2812)
#define LED_PRESENCE_DEFAULT_BLUE ((ws2812_rgb_t){.red = 0, .green = 0, .blue = 255})
#endif

#ifndef LED_PRESENCE_NEAR_THRESHOLD_CM
#define LED_PRESENCE_NEAR_THRESHOLD_CM 100.0f
#endif
#ifndef LED_PRESENCE_FAR_THRESHOLD_CM
#define LED_PRESENCE_FAR_THRESHOLD_CM 300.0f
#endif
#define LED_PRESENCE_FADE_DURATION_MS 2000

typedef enum
{
    LED_PRESENCE_AUTO_OFF_STATE_IDLE,
    LED_PRESENCE_AUTO_OFF_STATE_ACTIVE,
    LED_PRESENCE_AUTO_OFF_STATE_FADING,
    LED_PRESENCE_AUTO_OFF_STATE_OFF,
} led_presence_auto_off_state_t;

typedef enum
{
    LED_PRESENCE_STATE_UNKNOWN,
    LED_PRESENCE_STATE_NEAR,
    LED_PRESENCE_STATE_FAR,
} led_presence_distance_state_t;

static led_presence_auto_off_state_t s_auto_off_state     = LED_PRESENCE_AUTO_OFF_STATE_IDLE;
static uint32_t                      s_last_activity_time = 0;
static uint8_t                       s_fade_brightness    = 0;
static uint32_t                      s_fade_start_time    = 0;

static led_presence_distance_state_t s_presence_state   = LED_PRESENCE_STATE_UNKNOWN;
static float                         s_last_distance_cm = 0.0f;

esp_err_t led_presence_show(bool is_human_present)
{
    if (!led_controller_is_initialized())
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!is_human_present)
    {
        return led_controller_turn_off();
    }

#if (LED_TYPE == LED_TYPE_WS2812)
    return led_controller_set_color(LED_PRESENCE_DEFAULT_BLUE, LED_BREATH_MAX_BRIGHTNESS);
#else
    return ESP_OK;
#endif
}

esp_err_t led_presence_update_distance(float distance_cm)
{
    if (!led_controller_is_initialized())
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_last_distance_cm = distance_cm;

    led_presence_distance_state_t new_state = s_presence_state;

    if (distance_cm < LED_PRESENCE_NEAR_THRESHOLD_CM)
    {
        new_state = LED_PRESENCE_STATE_NEAR;
    }
    else if (distance_cm > LED_PRESENCE_FAR_THRESHOLD_CM)
    {
        new_state = LED_PRESENCE_STATE_FAR;
    }

    if (new_state != s_presence_state)
    {
        s_presence_state = new_state;

        if (new_state == LED_PRESENCE_STATE_NEAR)
        {
            ESP_LOGI(TAG, "Presence: NEAR (%.1f cm)", distance_cm);
            (void)led_presence_trigger_activity();
        }
        else if (new_state == LED_PRESENCE_STATE_FAR)
        {
            ESP_LOGI(TAG, "Presence: FAR (%.1f cm)", distance_cm);
        }
    }

    return ESP_OK;
}

esp_err_t led_presence_trigger_activity(void)
{
    if (!led_controller_is_initialized())
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_last_activity_time = esp_timer_get_time() / 1000;
    s_auto_off_state     = LED_PRESENCE_AUTO_OFF_STATE_ACTIVE;

    return ESP_OK;
}

esp_err_t led_presence_update_auto_off(uint32_t current_time_ms)
{
    if (!led_controller_is_initialized())
    {
        return ESP_ERR_INVALID_STATE;
    }

    led_module_snapshot_t snapshot = {0};
    esp_err_t             ret      = led_controller_get_snapshot(&snapshot);
    if (ret != ESP_OK)
    {
        return ret;
    }

    switch (s_auto_off_state)
    {
    case LED_PRESENCE_AUTO_OFF_STATE_IDLE:
        break;

    case LED_PRESENCE_AUTO_OFF_STATE_ACTIVE:
        if ((current_time_ms - s_last_activity_time) >= LED_AUTO_OFF_TIMEOUT_MS)
        {
            s_auto_off_state  = LED_PRESENCE_AUTO_OFF_STATE_FADING;
            s_fade_start_time = current_time_ms;
            s_fade_brightness = snapshot.brightness;
            ESP_LOGD(TAG, "Auto-off timeout, starting fade");
        }
        break;

    case LED_PRESENCE_AUTO_OFF_STATE_FADING:
    {
        uint32_t elapsed = current_time_ms - s_fade_start_time;
        if (elapsed >= LED_PRESENCE_FADE_DURATION_MS)
        {
            (void)led_controller_turn_off();
            s_auto_off_state = LED_PRESENCE_AUTO_OFF_STATE_OFF;
            ESP_LOGI(TAG, "LED auto-off completed");
        }
        else
        {
            float   fade_progress  = (float)elapsed / LED_PRESENCE_FADE_DURATION_MS;
            uint8_t new_brightness = (uint8_t)(s_fade_brightness * (1.0f - fade_progress));

            if (new_brightness != snapshot.brightness)
            {
#if (LED_TYPE == LED_TYPE_WS2812)
                ret = ws2812_module_set_brightness(new_brightness);
                if (ret != ESP_OK)
                {
                    ESP_LOGW(TAG, "Failed to set fade brightness: %s", esp_err_to_name(ret));
                }
#endif
            }
        }
    }
    break;

    case LED_PRESENCE_AUTO_OFF_STATE_OFF:
        break;

    default:
        ESP_LOGW(TAG, "Unknown auto-off state: %d", s_auto_off_state);
        s_auto_off_state = LED_PRESENCE_AUTO_OFF_STATE_IDLE;
        break;
    }

    return ESP_OK;
}

#endif
