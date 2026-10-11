/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file touch_handler.h
 * @brief Touch gesture behavior dispatcher
 *
 * Connects touch_interpreter gesture events to concrete actions:
 * - TAP: LED blink + screen welcome text + notify wifi_test to wake
 * - DOUBLE_TAP: brightness level cycling (delegated to brightness_controller)
 * - LONG_PRESS: turn off head light + ask voice board to end the conversation
 *
 * Requires touch_interpreter, led_controller, and display_system to be initialized.
 */

/**
 * @brief Initialize touch handler
 *
 * Initializes touch_sensor and touch_interpreter with gesture callbacks.
 * Registers REST API endpoint /api/touch/wake for wifi_test polling.
 *
 * @return ESP_OK on success
 */
esp_err_t touch_handler_init(void);

/**
 * @brief Deinitialize touch handler
 *
 * Stops touch_interpreter and releases resources.
 *
 * @return ESP_OK on success
 */
esp_err_t touch_handler_deinit(void);

/**
 * @brief Check if touch handler is initialized
 *
 * @return true if initialized
 */
bool touch_handler_is_initialized(void);

/**
 * @brief Register touch REST API endpoints on HTTP server
 *
 * Must be called after network_manager_init() (HTTP server is running).
 * Registers /api/touch/wake and /api/touch/brightness endpoints.
 */
void touch_handler_register_api(void);

#ifdef __cplusplus
}
#endif
