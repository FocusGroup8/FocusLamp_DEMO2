/*
 * focus_app.h - Focus timer application module for FocusLamp
 */

#pragma once
#ifndef __FOCUS_APP_H__
#define __FOCUS_APP_H__

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize focus application.
 *        Subscribes to EV_APP_MODE_CHANGED and focus timer events.
 * @return esp_err_t
 */
esp_err_t focus_app_init(void);

/**
 * @brief Start focus timer with specified duration.
 * @param duration_minutes  Focus session duration in minutes (e.g. 25 for Pomodoro)
 * @return esp_err_t
 */
esp_err_t focus_app_start(uint32_t duration_minutes);

/**
 * @brief Stop focus timer and reset state.
 * @return esp_err_t
 */
esp_err_t focus_app_stop(void);

/**
 * @brief Pause focus timer.
 * @return esp_err_t
 */
esp_err_t focus_app_pause(void);

/**
 * @brief Resume paused focus timer.
 * @return esp_err_t
 */
esp_err_t focus_app_resume(void);

/**
 * @brief Get remaining time in the current focus session.
 * @param[out] remaining_sec  Remaining time in seconds
 * @return esp_err_t
 */
esp_err_t focus_app_get_remaining(uint32_t *remaining_sec);

#ifdef __cplusplus
}
#endif

#endif /* __FOCUS_APP_H__ */