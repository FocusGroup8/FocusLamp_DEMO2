#ifndef LED_PRESENCE_H
#define LED_PRESENCE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "led_module_config.h"

#ifdef __cplusplus
extern "C"
{
#endif

#if (LED_MODULE_ENABLE == 1)

    esp_err_t led_presence_show(bool is_human_present);

    esp_err_t led_presence_update_distance(float distance_cm);

    esp_err_t led_presence_trigger_activity(void);

    esp_err_t led_presence_update_auto_off(uint32_t current_time_ms);

#else

static inline esp_err_t led_presence_show(bool is_human_present)
{
    (void)is_human_present;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t led_presence_update_distance(float distance_cm)
{
    (void)distance_cm;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t led_presence_trigger_activity(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t led_presence_update_auto_off(uint32_t current_time_ms)
{
    (void)current_time_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif

#ifdef __cplusplus
}
#endif

#endif
