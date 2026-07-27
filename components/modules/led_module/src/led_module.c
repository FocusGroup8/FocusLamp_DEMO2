#include "led_module.h"

#if (LED_MODULE_ENABLE == 1)

#include "esp_log.h"

#include "led_controller.h"
#include "led_effects.h"
#include "led_presence.h"

static const char* TAG = "led_module";

#if (LED_MODULE_LOG_ENABLE == 1)
#define LED_LOG_ENABLED 1
#else
#define LED_LOG_ENABLED 0
#endif

#define LED_LOGI(fmt, ...)                        \
    do                                            \
    {                                             \
        if (LED_LOG_ENABLED && s_led_log_enabled) \
            ESP_LOGI(TAG, fmt, ##__VA_ARGS__);    \
    } while (0)
#define LED_LOGD(fmt, ...)                        \
    do                                            \
    {                                             \
        if (LED_LOG_ENABLED && s_led_log_enabled) \
            ESP_LOGD(TAG, fmt, ##__VA_ARGS__);    \
    } while (0)

static bool s_led_log_enabled = false;

esp_err_t led_module_init(void)
{
    esp_err_t ret = led_controller_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize LED controller: %s", esp_err_to_name(ret));
        return ret;
    }

#if (LED_TYPE == LED_TYPE_WS2812)
    ws2812_rgb_t default_color = {.red = 0, .green = 0, .blue = 255};
    ret                        = led_controller_set_color(default_color, LED_BREATH_MAX_BRIGHTNESS);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set default color: %s", esp_err_to_name(ret));
        return ret;
    }
#endif

    s_led_log_enabled = false;

    ESP_LOGI(TAG, "LED module initialized");
    return ESP_OK;
}

esp_err_t led_module_deinit(void)
{
    return led_controller_deinit();
}

esp_err_t led_module_turn_off(void)
{
    return led_controller_turn_off();
}

esp_err_t led_module_set_solid_color(ws2812_rgb_t color, uint8_t brightness)
{
    return led_controller_set_color(color, brightness);
}

esp_err_t led_module_show_breath(uint32_t elapsed_ms)
{
    return led_effects_show_breath(elapsed_ms);
}

esp_err_t led_module_show_blink(led_blink_mode_t mode, uint32_t elapsed_ms)
{
    return led_effects_show_blink(mode, elapsed_ms);
}

esp_err_t led_module_show_presence(bool is_human_present)
{
    return led_presence_show(is_human_present);
}

esp_err_t led_module_show_heartbeat_level(uint8_t level)
{
    return led_effects_show_heartbeat_level(level);
}

esp_err_t led_module_show_sedentary_state(radar_motion_state_t state)
{
    return led_effects_show_sedentary_state(state);
}

esp_err_t led_module_show_system_state(led_module_system_state_t state)
{
    return led_effects_show_system_state(state);
}

esp_err_t led_module_get_snapshot(led_module_snapshot_t* snapshot)
{
    return led_controller_get_snapshot(snapshot);
}

bool led_module_is_initialized(void)
{
    return led_controller_is_initialized();
}

esp_err_t led_module_trigger_activity(void)
{
    return led_presence_trigger_activity();
}

esp_err_t led_module_update_auto_off(uint32_t current_time_ms)
{
    return led_presence_update_auto_off(current_time_ms);
}

esp_err_t led_module_update_presence_distance(float distance_cm)
{
    return led_presence_update_distance(distance_cm);
}

void led_module_enable_log(void)
{
#if (LED_LOG_ENABLED == 1)
    s_led_log_enabled = true;
    ESP_LOGI(TAG, "LED module log enabled");
#else
    ESP_LOGW(TAG, "LED module log is disabled in Kconfig");
#endif
}

void led_module_disable_log(void)
{
#if (LED_LOG_ENABLED == 1)
    s_led_log_enabled = false;
    ESP_LOGI(TAG, "LED module log disabled");
#else
    ESP_LOGW(TAG, "LED module log is disabled in Kconfig");
#endif
}

bool led_module_is_log_enabled(void)
{
    return s_led_log_enabled;
}

#endif
