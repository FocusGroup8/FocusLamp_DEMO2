/*
 * sensor_service.h - Sensor service for FocusLamp
 */

#pragma once
#ifndef __SENSOR_SERVICE_H__
#define __SENSOR_SERVICE_H__

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== Sensor Type Enumeration ===================== */
typedef enum {
    SENSOR_TYPE_AMBIENT_LIGHT = 0,
    SENSOR_TYPE_RADAR,
} sensor_type_t;

/* ===================== Radar Data Structure ===================== */
/* Forward declaration - use radar_driver's radar_data_t */
struct radar_data;

/* ===================== Sensor Callback ===================== */
typedef void (*sensor_cb_t)(sensor_type_t type, void *data, size_t data_size);

/**
 * @brief Initialize sensor service.
 *        Subscribes to EV_SENSOR_* events and initializes light_sensor_driver and radar_driver.
 * @return esp_err_t
 */
esp_err_t sensor_service_init(void);

/**
 * @brief Get ambient light level in lux.
 * @param[out] lux  Measured illuminance
 * @return esp_err_t
 */
esp_err_t sensor_service_get_light_lux(float *lux);

/**
 * @brief Get radar sensor data.
 * @param[out] data  Pointer to radar_data_t structure to fill
 * @return esp_err_t
 */
esp_err_t sensor_service_get_radar_data(struct radar_data *data);

/**
 * @brief Start periodic sensor monitoring.
 * @return esp_err_t
 */
esp_err_t sensor_service_start_monitoring(void);

/**
 * @brief Stop sensor monitoring.
 * @return esp_err_t
 */
esp_err_t sensor_service_stop_monitoring(void);

/**
 * @brief Set the sensor sampling interval.
 * @param interval_ms  Sampling interval in milliseconds (minimum 10 ms)
 * @return esp_err_t
 */
esp_err_t sensor_service_set_sampling_interval(uint32_t interval_ms);

/**
 * @brief Feed a raw light sensor sample for averaging and smoothing.
 *        Called periodically (e.g. every 100ms) by sensor_task.
 *        Internal PID-like filter accumulates 10-second block averages,
 *        applies EMA smoothing, and publishes a smoothed value once per minute.
 * @param lux  Raw lux reading
 */
void sensor_service_feed_sample(float lux);

/**
 * @brief Get the current smoothed (EMA-filtered) lux value.
 * @param[out] lux  Smoothed illuminance
 * @return esp_err_t
 */
esp_err_t sensor_service_get_smoothed_lux(float *lux);

/**
 * @brief Convert lux value to brightness level (0-4).
 * @param lux  Illuminance in lux
 * @return uint8_t  Level 0 (dark) through 4 (very bright)
 */
uint8_t sensor_service_lux_to_level(float lux);

/**
 * @brief Register a callback for sensor data updates.
 * @param type  Sensor type to monitor
 * @param cb    Callback function
 * @return esp_err_t
 */
esp_err_t sensor_service_set_callback(sensor_type_t type, sensor_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* __SENSOR_SERVICE_H__ */