/*
 * music_rhythm_app.h - Music rhythm application module for FocusLamp
 */

#pragma once
#ifndef __MUSIC_RHYTHM_APP_H__
#define __MUSIC_RHYTHM_APP_H__

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize music rhythm application.
 *        Subscribes to EV_AUDIO__DATA and EV_APP_MODE_CHANGED events.
 * @return esp_err_t
 */
esp_err_t music_rhythm_app_init(void);

/**
 * @brief Start music rhythm mode.
 *        Begins listening for audio data and controlling LED/arm effects.
 * @return esp_err_t
 */
esp_err_t music_rhythm_app_start(void);

/**
 * @brief Stop music rhythm mode.
 * @return esp_err_t
 */
esp_err_t music_rhythm_app_stop(void);

/**
 * @brief Set sensitivity level for rhythm detection.
 * @param level  Sensitivity level (0-10, higher = more responsive)
 * @return esp_err_t
 */
esp_err_t music_rhythm_app_set_sensitivity(uint8_t level);

#ifdef __cplusplus
}
#endif

#endif /* __MUSIC_RHYTHM_APP_H__ */