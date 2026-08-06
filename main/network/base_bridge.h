/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file base_bridge.h
 * @brief HTTP bridge from head board to base board (three-board demo)
 *
 * Provides outbound REST calls to the base board:
 *   - GET  /api/ambient        : query ambient light level (0-4)
 *   - POST /api/arm/gesture    : arm single-step action (thumb_up / thumb_down)
 *   - POST /api/detect/phone   : forward VLM phone/computer detection result
 *
 * Calls are synchronous with a short timeout; failures are logged and
 * reported via return code without blocking the caller's flow.
 */

/**
 * @brief Query ambient light level from the base board
 *
 * @param level_out Output: ambient light level 0-4 (valid only on ESP_OK)
 * @return ESP_OK on success (level parsed), ESP_ERR_INVALID_ARG on null arg,
 *         other esp_err_t on network/HTTP/parse failure
 */
esp_err_t base_bridge_get_ambient(int *level_out);

/**
 * @brief Request an arm gesture action on the base board
 *
 * @param gesture "thumb_up" or "thumb_down"
 * @return ESP_OK on success, otherwise an esp_err_t error code
 */
esp_err_t base_bridge_post_arm_gesture(const char *gesture);

/**
 * @brief Forward a VLM phone/computer detection to the base board
 *
 * @param source "phone" or "computer"
 * @return ESP_OK on success, otherwise an esp_err_t error code
 */
esp_err_t base_bridge_post_detect_phone(const char *source);

#ifdef __cplusplus
}
#endif
