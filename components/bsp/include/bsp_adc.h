/*
 * bsp_adc.h - ADC initialization and reading for FocusLamp BSP
 */

#pragma once
#ifndef __BSP_ADC_H__
#define __BSP_ADC_H__

#include <stdint.h>
#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize ADC for ambient light sensor on GPIO21 (ADC1_CH5)
 *        Uses adc_oneshot driver.
 */
esp_err_t bsp_adc_init(void);

/**
 * @brief Read ambient light sensor ADC value
 * @param[out] value  Pointer to store the raw ADC reading
 * @return esp_err_t
 */
esp_err_t bsp_adc_read_light(uint32_t *value);

/**
 * @brief Get the singleton ADC handle (created by bsp_adc_init).
 * @return adc_oneshot_unit_handle_t, or NULL if not initialized.
 */
adc_oneshot_unit_handle_t bsp_adc_get_handle(void);

/**
 * @brief Get the configured ADC channel for the light sensor.
 * @return adc_channel_t
 */
adc_channel_t bsp_adc_get_light_channel(void);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_ADC_H__ */
