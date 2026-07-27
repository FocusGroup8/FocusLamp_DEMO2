/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "status_reporter_config.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#if (STATUS_REPORTER_ENABLE == 1)

/**
 * @brief Event types that trigger immediate status report
 */
typedef enum {
    STATUS_EVENT_NONE    = 0,
    STATUS_EVENT_LED     = 1,
    STATUS_EVENT_SERVO   = 2,
    STATUS_EVENT_LCD     = 3,
    STATUS_EVENT_DISPLAY = 4,
} status_event_type_t;

/**
 * @brief Initialize status reporter
 *
 * Starts periodic timer and prepares HTTP client for sending
 * status reports to wifi_test.
 *
 * @return ESP_OK on success, error code on failure
 */
esp_err_t status_reporter_init(void);

/**
 * @brief Deinitialize status reporter
 */
void status_reporter_deinit(void);

/**
 * @brief Notify status reporter of a state change event
 *
 * Triggers an immediate status report (subject to debouncing).
 * This function is safe to call from any task.
 *
 * @param event_type  Type of event that triggered the report
 */
void status_reporter_notify_event(status_event_type_t event_type);

/**
 * @brief Force send a status report now (bypasses debounce)
 *
 * @param trigger  Trigger reason string ("timer" or "event")
 * @return ESP_OK on success, error code on failure
 */
esp_err_t status_reporter_send_now(const char *trigger);

#else /* STATUS_REPORTER_ENABLE == 0 */

/* Stub implementations when disabled */
static inline esp_err_t status_reporter_init(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline void status_reporter_deinit(void) {}
static inline void status_reporter_notify_event(status_event_type_t event_type) { (void)event_type; }
static inline esp_err_t status_reporter_send_now(const char *trigger) { (void)trigger; return ESP_ERR_NOT_SUPPORTED; }

#endif /* STATUS_REPORTER_ENABLE */

#ifdef __cplusplus
}
#endif
