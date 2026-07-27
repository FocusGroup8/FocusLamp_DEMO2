#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "led_module_config.h"
#include "led_types.h"

#if (LED_MODULE_ENABLE == 1)

#if (LED_TYPE == LED_TYPE_WS2812)
#include "ws2812_module.h"
#endif

#endif

#ifdef __cplusplus
extern "C"
{
#endif

#if (LED_MODULE_ENABLE == 1)

    esp_err_t led_module_init(void);

    esp_err_t led_module_deinit(void);

    esp_err_t led_module_turn_off(void);

    esp_err_t led_module_set_solid_color(ws2812_rgb_t color, uint8_t brightness);

    esp_err_t led_module_show_breath(uint32_t elapsed_ms);

    esp_err_t led_module_show_blink(led_blink_mode_t mode, uint32_t elapsed_ms);

    esp_err_t led_module_show_presence(bool is_human_present);

    esp_err_t led_module_show_heartbeat_level(uint8_t level);

    esp_err_t led_module_show_sedentary_state(radar_motion_state_t state);

    esp_err_t led_module_show_system_state(led_module_system_state_t state);

    esp_err_t led_module_get_snapshot(led_module_snapshot_t* snapshot);

    bool led_module_is_initialized(void);

    esp_err_t led_module_update_auto_off(uint32_t current_time_ms);

    esp_err_t led_module_trigger_activity(void);

    esp_err_t led_module_update_presence_distance(float distance_cm);

    void led_module_enable_log(void);
    void led_module_disable_log(void);
    bool led_module_is_log_enabled(void);

#else

static inline esp_err_t led_module_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t led_module_deinit(void)
{
    return ESP_OK;
}
static inline esp_err_t led_module_turn_off(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t led_module_set_solid_color(ws2812_rgb_t color, uint8_t brightness)
{
    (void)color;
    (void)brightness;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t led_module_show_breath(uint32_t elapsed_ms)
{
    (void)elapsed_ms;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t led_module_show_blink(led_blink_mode_t mode, uint32_t elapsed_ms)
{
    (void)mode;
    (void)elapsed_ms;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t led_module_show_presence(bool is_human_present)
{
    (void)is_human_present;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t led_module_show_heartbeat_level(uint8_t level)
{
    (void)level;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t led_module_show_sedentary_state(radar_motion_state_t state)
{
    (void)state;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t led_module_show_system_state(led_module_system_state_t state)
{
    (void)state;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t led_module_get_snapshot(led_module_snapshot_t* snapshot)
{
    (void)snapshot;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline bool led_module_is_initialized(void)
{
    return false;
}
static inline esp_err_t led_module_update_auto_off(uint32_t current_time_ms)
{
    (void)current_time_ms;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t led_module_trigger_activity(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t led_module_update_presence_distance(float distance_cm)
{
    (void)distance_cm;
    return ESP_ERR_NOT_SUPPORTED;
}

static inline void led_module_enable_log(void)
{
}
static inline void led_module_disable_log(void)
{
}
static inline bool led_module_is_log_enabled(void)
{
    return false;
}

#endif

#ifdef __cplusplus
}
#endif
