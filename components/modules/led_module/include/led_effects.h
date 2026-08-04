#ifndef LED_EFFECTS_H
#define LED_EFFECTS_H

#include <stdint.h>

#include "esp_err.h"

#include "led_module_config.h"
#include "led_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

#if (LED_MODULE_ENABLE == 1)

    esp_err_t led_effects_show_breath(uint32_t elapsed_ms);

    esp_err_t led_effects_show_blink(led_blink_mode_t mode, uint32_t elapsed_ms);

    esp_err_t led_effects_show_heartbeat_level(uint8_t level);

    esp_err_t led_effects_show_sedentary_state(radar_motion_state_t state);

    esp_err_t led_effects_show_system_state(led_module_system_state_t state);

#else

static inline esp_err_t led_effects_show_breath(uint32_t elapsed_ms)
{
    (void)elapsed_ms;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t led_effects_show_blink(led_blink_mode_t mode, uint32_t elapsed_ms)
{
    (void)mode;
    (void)elapsed_ms;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t led_effects_show_heartbeat_level(uint8_t level)
{
    (void)level;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t led_effects_show_sedentary_state(radar_motion_state_t state)
{
    (void)state;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t led_effects_show_system_state(led_module_system_state_t state)
{
    (void)state;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif

#ifdef __cplusplus
}
#endif

#endif
