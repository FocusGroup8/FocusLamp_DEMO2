/*
 * companion_app.h - Companion application module for FocusLamp
 */

#pragma once
#ifndef __COMPANION_APP_H__
#define __COMPANION_APP_H__

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize companion application.
 *        Subscribes to EV_APP_MODE_CHANGED and related events.
 * @return esp_err_t
 */
esp_err_t companion_app_init(void);

/**
 * @brief Start companion mode.
 *        Enables RGB gradual lighting, LCD expression, and arm friendly actions.
 * @return esp_err_t
 */
esp_err_t companion_app_start(void);

/**
 * @brief Stop companion mode and restore default state.
 * @return esp_err_t
 */
esp_err_t companion_app_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* __COMPANION_APP_H__ */