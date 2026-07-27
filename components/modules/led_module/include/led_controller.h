#ifndef LED_CONTROLLER_H
#define LED_CONTROLLER_H

#include <stdint.h>

#include "esp_err.h"

#include "led_module_config.h"
#include "led_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

#if (LED_MODULE_ENABLE == 1)

    esp_err_t led_controller_init(void);

    esp_err_t led_controller_deinit(void);

    esp_err_t led_controller_turn_off(void);

    esp_err_t led_controller_set_color(ws2812_rgb_t color, uint8_t brightness);

    esp_err_t led_controller_get_snapshot(led_module_snapshot_t* snapshot);

    bool led_controller_is_initialized(void);

#else

static inline esp_err_t led_controller_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t led_controller_deinit(void)
{
    return ESP_OK;
}
static inline esp_err_t led_controller_turn_off(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t led_controller_set_color(ws2812_rgb_t color, uint8_t brightness)
{
    (void)color;
    (void)brightness;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t led_controller_get_snapshot(led_module_snapshot_t* snapshot)
{
    (void)snapshot;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline bool led_controller_is_initialized(void)
{
    return false;
}

#endif

#ifdef __cplusplus
}
#endif

#endif
