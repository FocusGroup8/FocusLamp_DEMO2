/**
 * @file light_sensor_driver_config.h
 * @brief Light sensor driver configuration header
 *
 * This file maps Kconfig options to C macros for the light sensor driver.
 *
 * @author CottonLin
 * @date 2026-04-28
 * @version 1.0.0
 */

#pragma once

#include "esp_adc/adc_oneshot.h"

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @defgroup LightSensorConfig Light Sensor Configuration
 * @brief Configuration macros for light sensor driver
 *
 * Defaults are hardcoded when Kconfig is not configured.
 * When Kconfig symbols are defined, they take precedence.
 * @{
 */

/* ===================== Enable / Disable ===================== */
#ifndef CONFIG_PROJECT_ENABLE_LIGHT_SENSOR
#define CONFIG_PROJECT_ENABLE_LIGHT_SENSOR 1
#endif
#define LIGHT_SENSOR_ENABLE CONFIG_PROJECT_ENABLE_LIGHT_SENSOR

#if (LIGHT_SENSOR_ENABLE == 1)

/* ===================== ADC Hardware ===================== */
#ifndef CONFIG_LIGHT_SENSOR_ADC_UNIT
#define CONFIG_LIGHT_SENSOR_ADC_UNIT 1       /* ADC_UNIT_1 */
#endif

#ifndef CONFIG_LIGHT_SENSOR_ADC_CHANNEL
#define CONFIG_LIGHT_SENSOR_ADC_CHANNEL 5    /* ADC1_CH5 = GPIO21 */
#endif

#ifndef CONFIG_LIGHT_SENSOR_ADC_ATTEN
#define CONFIG_LIGHT_SENSOR_ADC_ATTEN 3      /* ADC_ATTEN_DB_12 (0-3.3V) */
#endif

/** @brief ADC unit number */
#define LIGHT_SENSOR_ADC_UNIT ((adc_unit_t)CONFIG_LIGHT_SENSOR_ADC_UNIT)

/** @brief ADC channel number */
#define LIGHT_SENSOR_ADC_CHANNEL ((adc_channel_t)CONFIG_LIGHT_SENSOR_ADC_CHANNEL)

/** @brief ADC attenuation level */
#define LIGHT_SENSOR_ADC_ATTEN ((adc_atten_t)CONFIG_LIGHT_SENSOR_ADC_ATTEN)

/* ===================== Lux Thresholds ===================== */
#ifndef CONFIG_LIGHT_SENSOR_LUX_THRESHOLD_LOW
#define CONFIG_LIGHT_SENSOR_LUX_THRESHOLD_LOW 2000    /* 20.00 lux (dark boundary) */
#endif

#ifndef CONFIG_LIGHT_SENSOR_LUX_THRESHOLD_HIGH
#define CONFIG_LIGHT_SENSOR_LUX_THRESHOLD_HIGH 100000 /* 1000.00 lux (bright boundary) */
#endif

/** @brief Low lux threshold for turning on the light */
#define LIGHT_SENSOR_LUX_THRESHOLD_LOW ((float)CONFIG_LIGHT_SENSOR_LUX_THRESHOLD_LOW / 100.0f)

/** @brief High lux threshold for turning off the light */
#define LIGHT_SENSOR_LUX_THRESHOLD_HIGH ((float)CONFIG_LIGHT_SENSOR_LUX_THRESHOLD_HIGH / 100.0f)

#endif

#ifdef __cplusplus
}
#endif
