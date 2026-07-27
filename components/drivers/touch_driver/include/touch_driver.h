/*
 * touch_driver.h - Capacitive touch sensor driver for FocusLamp
 * Uses external TTP capacitive touch chip, output is GPIO level (active high).
 */

#pragma once
#ifndef __TOUCH_DRIVER_H__
#define __TOUCH_DRIVER_H__

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Touch point enumeration (TTP A/B/C/D) */
typedef enum {
    TOUCH_POINT_A = 0,
    TOUCH_POINT_B,
    TOUCH_POINT_C,
    TOUCH_POINT_D,
    TOUCH_POINT_MAX,
} touch_point_t;

/* Debounce configuration */
#define TOUCH_DRIVER_DEBOUNCE_COUNT 3

/**
 * @brief Initialize touch driver.
 *        Configures 4 GPIOs (TTP_A/B/C/D) as inputs.
 * @return esp_err_t
 */
esp_err_t touch_driver_init(void);

/**
 * @brief Scan all touch points and update internal state.
 *        Should be called periodically (e.g., via software timer).
 */
void touch_driver_scan(void);

/**
 * @brief Get the current state of a specific touch point.
 * @param point  Touch point to query
 * @return true  = touched (GPIO high)
 * @return false = not touched (GPIO low)
 */
bool touch_driver_get_state(touch_point_t point);

/**
 * @brief Get a bitmask of all currently active (touched) touch points.
 * @return uint8_t  Bitmask: bit0=TTP_A, bit1=TTP_B, bit2=TTP_C, bit3=TTP_D
 */
uint8_t touch_driver_get_active_points(void);

#ifdef __cplusplus
}
#endif

#endif /* __TOUCH_DRIVER_H__ */