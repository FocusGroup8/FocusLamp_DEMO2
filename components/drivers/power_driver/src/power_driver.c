/*
 * power_driver.c - Power management driver implementation
 */

#include "power_driver.h"
#include "bsp_power.h"

esp_err_t power_driver_init(void)
{
    return bsp_power_init();
}

esp_err_t power_driver_set_esp_power(bool on)
{
    return bsp_power_set_esp_enable(on);
}
