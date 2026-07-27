/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Display interface for xiaozhi manager
 *
 * Pre-defined interface for screen display integration.
 * Implementation should be provided by the display module.
 * Currently stubs; real implementation to be added when screen hardware is available.
 */

/**
 * @brief  Display event types
 */
typedef enum {
    XIAOZHI_DISPLAY_EVENT_STATE_CHANGED,       /*!< Device state changed */
    XIAOZHI_DISPLAY_EVENT_TASK_CREATED,        /*!< Task created */
    XIAOZHI_DISPLAY_EVENT_TASK_UPDATED,        /*!< Task updated (countdown, etc.) */
    XIAOZHI_DISPLAY_EVENT_TASK_COMPLETED,      /*!< Task completed */
    XIAOZHI_DISPLAY_EVENT_TTS_TEXT,            /*!< TTS text received */
} xiaozhi_display_event_t;

/**
 * @brief  Display callback for xiaozhi events
 *
 * @param event   Display event type
 * @param data    Event-specific data (string or struct pointer)
 * @return ESP_OK on success
 */
typedef esp_err_t (*xiaozhi_display_cb_t)(xiaozhi_display_event_t event, void *data);

/**
 * @brief  Register display callback
 *
 * @param cb  Display callback function
 * @return ESP_OK on success
 */
esp_err_t xiaozhi_display_register_callback(xiaozhi_display_cb_t cb);

/**
 * @brief  Notify display of an event
 *
 * @param event   Display event type
 * @param data    Event data
 * @return ESP_OK on success
 */
esp_err_t xiaozhi_display_notify(xiaozhi_display_event_t event, void *data);

#ifdef __cplusplus
}
#endif
