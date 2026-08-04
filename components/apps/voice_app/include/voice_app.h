/*
 * voice_app.h - Voice control application module for FocusLamp
 */

#pragma once
#ifndef __VOICE_APP_H__
#define __VOICE_APP_H__

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize voice control application.
 *        Subscribes to EV_APP_MODE_CHANGED and voice recognition events.
 * @return esp_err_t
 */
esp_err_t voice_app_init(void);

/**
 * @brief Start voice control mode.
 * @return esp_err_t
 */
esp_err_t voice_app_start(void);

/**
 * @brief Stop voice control mode.
 * @return esp_err_t
 */
esp_err_t voice_app_stop(void);

/**
 * @brief Process a voice recognition result as a text command.
 * @param text  Null-terminated string containing the recognized command
 * @return esp_err_t
 */
esp_err_t voice_app_process_command(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* __VOICE_APP_H__ */