/*
 * bsp_power.c - Power management for FocusLamp BSP
 */

#include "bsp_power.h"
#include "bsp_gpio.h"
#include "pin_config.h"

esp_err_t bsp_power_init(void)
{
    esp_err_t ret;

    /* Ensure GPIOs are initialized first */
    ret = bsp_gpio_init();
    if (ret != ESP_OK) return ret;

    /* Set ESP_EN high to enable ESP module */
    ret = bsp_gpio_set_level(ESP_EN_GPIO, 1);
    if (ret != ESP_OK) return ret;

    return ESP_OK;
}

esp_err_t bsp_power_set_esp_enable(bool enable)
{
    return bsp_gpio_set_level(ESP_EN_GPIO, enable ? 1 : 0);
}
