/*
 * arm_action_app.h - Mechanical arm action application module for FocusLamp
 */

#pragma once
#ifndef __ARM_ACTION_APP_H__
#define __ARM_ACTION_APP_H__

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "arm_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== Predefined Action IDs ===================== */
typedef enum {
    ARM_ACTION_WAVE     = 0,    /* 挥手 */
    ARM_ACTION_NOD,             /* 点头 */
    ARM_ACTION_SHAKE,           /* 摇头 */
    ARM_ACTION_POINT,           /* 指向 */
    ARM_ACTION_HUG,             /* 拥抱 */
    ARM_ACTION_MAX,
} arm_action_id_t;

/**
 * @brief Initialize arm action application.
 *        Subscribes to EV_APP_MODE_CHANGED and related events.
 * @return esp_err_t
 */
esp_err_t arm_action_app_init(void);

/**
 * @brief Play a predefined action.
 * @param action_id  Action identifier to execute
 * @return esp_err_t
 */
esp_err_t arm_action_app_play(arm_action_id_t action_id);

/**
 * @brief Play a custom action sequence.
 * @param seq  Pointer to action sequence definition
 * @return esp_err_t
 */
esp_err_t arm_action_app_play_custom(const action_sequence_t *seq);

/**
 * @brief Emergency stop current action.
 * @return esp_err_t
 */
esp_err_t arm_action_app_stop(void);

/**
 * @brief Start learn mode - records servo positions for later playback.
 * @return esp_err_t
 */
esp_err_t arm_action_app_learn_start(void);

/**
 * @brief Stop learn mode and save recorded sequence.
 * @return esp_err_t
 */
esp_err_t arm_action_app_learn_stop(void);

/**
 * @brief Save a learned action sequence by name.
 * @param name  Name to save the action as
 * @return esp_err_t
 */
esp_err_t arm_action_app_save(const char *name);

/**
 * @brief Load a previously saved action sequence by name.
 * @param name  Name of the action to load
 * @return esp_err_t
 */
esp_err_t arm_action_app_load(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* __ARM_ACTION_APP_H__ */