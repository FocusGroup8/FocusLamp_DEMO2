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
 * @file tts_inject.h
 * @brief HTTP bridge from head board to voice board (welcome TTS injection)
 *
 * Sends POST /api/tts/speak to the voice board with body
 * {"text": "...", "priority": 1} to play a welcome message through the
 * voice board's speaker. Synchronous with a short timeout; failures are
 * logged and reported via return code.
 */

/**
 * @brief Ask the voice board to speak the given text
 *
 * @param text UTF-8 text to broadcast
 * @return ESP_OK on success, otherwise an esp_err_t error code
 */
esp_err_t tts_inject_speak(const char *text);

#ifdef __cplusplus
}
#endif
