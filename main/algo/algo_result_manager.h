/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#include <stdbool.h>

/* Forward declaration: cJSON type (full definition in cJSON.h, included by .c) */
typedef struct cJSON cJSON;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file algo_result_manager.h
 * @brief Algorithm result consumer for head board (camera-fps-optimization)
 *
 * Receives algorithm.result JSON-RPC from docker via /algo WebSocket,
 * dispatches to 5 sub-handlers (emotion/focus/vlm_game/presence/gesture),
 * and forwards TTS/arm/phone-detection to external boards via HTTP bridges:
 *   - TTS  → voice board via tts_inject_speak()
 *   - arm  → base board via base_bridge_post_arm_gesture()
 *   - VLM  → base board via base_bridge_post_detect_phone()
 *
 * Mode state machine (Normal/Focus/Companion) controls sub-handler enablement.
 * Emotion→eyes is handled locally via expressive_eyes_set_expression().
 */

/**
 * @brief Application mode for algo result processing
 */
typedef enum {
    ALGO_MODE_NORMAL = 0, /*!< Default: all sub-handlers except gesture */
    ALGO_MODE_FOCUS,      /*!< Focus mode: emotion/focus/vlm_game/presence, 30min countdown */
    ALGO_MODE_COMPANION,  /*!< Companion mode: emotion/focus/gesture, suppress vlm_game/presence */
} algo_mode_t;

/**
 * @brief Initialize algo result manager
 *
 * @return ESP_OK on success
 */
esp_err_t algo_result_manager_init(void);

/**
 * @brief Handle algorithm result arguments (called by mcp_cb_algo_result)
 *
 * Parses the arguments JSON and dispatches to 5 sub-handlers based on current mode.
 *
 * @param arguments  cJSON object of algorithm.result arguments (may be NULL)
 * @return ESP_OK on success
 */
esp_err_t algo_result_handle(const cJSON *arguments);

/**
 * @brief Set the current application mode
 *
 * Resets sedentary timer and all TTS cooldowns on mode change.
 *
 * @param mode  New mode to switch to
 */
void algo_result_set_mode(algo_mode_t mode);

#ifdef __cplusplus
}
#endif
