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
    SENSOR_TYPE_RADAR = 0,
} sensor_type_t;

/* ===================== Radar Data Structure ===================== */
/* Forward declaration - use radar_driver's radar_data_t */
struct radar_data;

/* ===================== Sensor Callback ===================== */
typedef void (*sensor_cb_t)(sensor_type_t type, void *data, size_t data_size);

/**
 * @brief Initialize sensor service.
 *        Subscribes to EV_SENSOR_* events and initializes radar_driver.
 *        (The ambient light sensor module has been removed.)
 * @return esp_err_t
 */
esp_err_t sensor_service_init(void);

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