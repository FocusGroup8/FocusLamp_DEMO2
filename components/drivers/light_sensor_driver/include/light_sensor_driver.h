/**
 * @file light_sensor_driver.h
 * @brief Light sensor driver interface
 *
 * This file provides the interface for the TEMT6000 light sensor driver,
 * which measures ambient light intensity using ADC.
 *
 * The driver provides:
 * - Sensor initialization and deinitialization
 * - Light intensity reading (lux)
 * - Threshold-based light state detection
 *
 * Configuration is managed through Kconfig system:
 * - PROJECT_ENABLE_LIGHT_SENSOR: Enable/disable the driver
 * - LIGHT_SENSOR_ADC_UNIT: ADC unit number
 * - LIGHT_SENSOR_ADC_CHANNEL: ADC channel number
 * - LIGHT_SENSOR_ADC_ATTEN: ADC attenuation level
 * - LIGHT_SENSOR_LUX_THRESHOLD_LOW: Low lux threshold
 * - LIGHT_SENSOR_LUX_THRESHOLD_HIGH: High lux threshold
 *
 * @author CottonLin
 * @date 2026-04-28
 * @version 1.0.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "light_sensor_driver_config.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @defgroup LightSensorDriver Light Sensor Driver
     * @brief TEMT6000 light sensor driver interface
     * @{
     */

#if (CONFIG_PROJECT_ENABLE_LIGHT_SENSOR == 1)

    /**
     * @brief Initialize the light sensor driver
     *
     * This function initializes the ADC peripheral and configures the
     * light sensor with parameters from Kconfig.
     *
     * @note This function must be called before any other driver functions
     * @note Configuration is read from Kconfig
     *
     * @return esp_err_t
     *      - ESP_OK: Success
     *      - ESP_ERR_INVALID_STATE: Driver already initialized
     *      - ESP_ERR_NO_MEM: Memory allocation failed
     *      - Other error codes from ADC driver
     */
    esp_err_t light_sensor_driver_init(void);

    /**
     * @brief Deinitialize the light sensor driver and release ADC resources
     *
     * This function stops the ADC peripheral and releases all resources.
     *
     * @return esp_err_t
     *      - ESP_OK: Success
     *      - ESP_ERR_INVALID_STATE: Driver not initialized
     */
    esp_err_t light_sensor_driver_deinit(void);

    /**
     * @brief Read light intensity from the sensor
     *
     * This function reads the ADC value and converts it to lux (light intensity).
     *
     * @param[out] lux Pointer to store the light intensity value in lux
     *
     * @return esp_err_t
     *      - ESP_OK: Success
     *      - ESP_ERR_INVALID_STATE: Driver not initialized
     *      - ESP_ERR_INVALID_ARG: Invalid parameter
     *      - Other error codes from ADC driver
     */
    esp_err_t light_sensor_driver_read(float* lux);

    /**
     * @brief Read light intensity with multiple sampling and averaging
     *
     * @param[out] lux Pointer to store the averaged light intensity value in lux
     * @param[in] sample_count Number of samples to average
     *
     * @return esp_err_t
     *      - ESP_OK: Success
     *      - ESP_ERR_INVALID_STATE: Driver not initialized
     *      - ESP_ERR_INVALID_ARG: Invalid parameter
     *      - ESP_FAIL: All samples failed
     */
    esp_err_t light_sensor_driver_read_averaged(float* lux, uint8_t sample_count);

    /**
     * @brief Check if the environment is dark (lux < threshold_low)
     *
     * @param[out] is_dark Pointer to store the result
     *
     * @return esp_err_t
     *      - ESP_OK: Success
     *      - ESP_ERR_INVALID_STATE: Driver not initialized
     *      - ESP_ERR_INVALID_ARG: Invalid parameter
     */
    esp_err_t light_sensor_driver_is_dark(bool* is_dark);

    /**
     * @brief Check if the environment is bright (lux > threshold_high)
     *
     * @param[out] is_bright Pointer to store the result
     *
     * @return esp_err_t
     *      - ESP_OK: Success
     *      - ESP_ERR_INVALID_STATE: Driver not initialized
     *      - ESP_ERR_INVALID_ARG: Invalid parameter
     */
    esp_err_t light_sensor_driver_is_bright(bool* is_bright);

    /**
     * @brief Check if the driver is initialized
     *
     * @return true if initialized, false otherwise
     */
    bool light_sensor_driver_is_initialized(void);

#else

static inline esp_err_t light_sensor_driver_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t light_sensor_driver_deinit(void)
{
    return ESP_OK;
}
static inline esp_err_t light_sensor_driver_read(float* lux)
{
    (void)lux;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t light_sensor_driver_read_averaged(float* lux, uint8_t sample_count)
{
    (void)lux;
    (void)sample_count;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t light_sensor_driver_is_dark(bool* is_dark)
{
    (void)is_dark;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t light_sensor_driver_is_bright(bool* is_bright)
{
    (void)is_bright;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline bool light_sensor_driver_is_initialized(void)
{
    return false;
}

#endif

#ifdef __cplusplus
}
#endif