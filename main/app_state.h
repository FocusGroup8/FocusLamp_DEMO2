/*
 * app_state.h - Application state machine for FocusLamp
 */

#pragma once
#ifndef __APP_STATE_H__
#define __APP_STATE_H__

#include <stdint.h>
#include "esp_err.h"
#include "event_bus.h"
#include "device_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== State Change Callback ===================== */
typedef void (*state_change_handler_t)(app_state_t old_state, app_state_t new_state, void *context);

/* ===================== State Manager API ===================== */

/**
 * @brief Initialize the state manager.
 *        Sets initial state to APP_STATE_INIT.
 * @return esp_err_t
 */
esp_err_t app_state_manager_init(void);

/**
 * @brief Transition to a new application state.
 *        Publishes EV_APP_MODE_CHANGED event on state change.
 * @param state  Target state
 * @return esp_err_t
 */
esp_err_t app_state_manager_set_state(app_state_t state);

/**
 * @brief Get the current application state.
 * @return Current app_state_t value
 */
app_state_t app_state_manager_get_state(void);

/**
 * @brief Register a callback for state transitions.
 * @param state    State to monitor (or all states)
 * @param handler  Callback function
 * @param context  User context passed to callback
 * @return esp_err_t
 */
esp_err_t app_state_manager_register_handler(app_state_t state, state_change_handler_t handler, void *context);

#ifdef __cplusplus
}
#endif

#endif /* __APP_STATE_H__ */