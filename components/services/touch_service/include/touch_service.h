/*
 * touch_service.h - Touch service for FocusLamp
 */

#pragma once
#ifndef __TOUCH_SERVICE_H__
#define __TOUCH_SERVICE_H__

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "touch_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize touch service.
 *        Subscribes to EV_TOUCH_* events and initializes touch_driver.
 * @return esp_err_t
 */
esp_err_t touch_service_init(void);

/**
 * @brief Start touch scanning.
 *        Begins periodic touch point sampling and gesture detection.
 * @return esp_err_t
 */
esp_err_t touch_service_start(void);

/**
 * @brief Stop touch scanning.
 * @return esp_err_t
 */
esp_err_t touch_service_stop(void);

/**
 * @brief Process one scan cycle. Should be called periodically (e.g. every 10ms).
 *        Handles debounce, long press, double click, release and combo detection.
 */
void touch_service_process(void);

/**
 * @brief Get current stable active points bitmask.
 * @return uint8_t Bitmask of active points
 */
uint8_t touch_service_get_active_points(void);

/**
 * @brief Check if a combo of two points is currently active.
 * @param point_a First point
 * @param point_b Second point
 * @return true if both are active
 */
bool touch_service_is_combo_active(touch_point_t point_a, touch_point_t point_b);

#ifdef __cplusplus
}
#endif

#endif /* __TOUCH_SERVICE_H__ */