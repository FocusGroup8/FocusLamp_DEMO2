/*
 * exercise_follow_app.h - Exercise follow mode for FocusLamp
 */

#pragma once
#ifndef __EXERCISE_FOLLOW_APP_H__
#define __EXERCISE_FOLLOW_APP_H__

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== Exercise Action IDs ===================== */
typedef enum {
    EXERCISE_ACTION_STRETCH_ARM    = 0,   /* 伸展手臂 */
    EXERCISE_ACTION_STRETCH_NECK,         /* 颈部伸展 */
    EXERCISE_ACTION_SHOULDER_RELAX,       /* 放松肩膀 */
    EXERCISE_ACTION_TWIST_WAIST,          /* 腰部扭转 */
    EXERCISE_ACTION_EYE_RELAX,            /* 眼部放松 */
    EXERCISE_ACTION_DEEP_BREATH,          /* 深呼吸 */
    EXERCISE_ACTION_MAX,
} exercise_action_id_t;

/* ===================== Exercise Session Config ===================== */
typedef struct {
    uint32_t            total_duration_sec;  /* Total exercise duration in seconds */
    uint8_t             interval_count;      /* How many intervals (e.g. 3 breaks) */
    uint32_t            work_duration_sec;   /* Work period before break */
    bool                voice_guide_enable;  /* Enable voice guidance */
    bool                arm_demo_enable;     /* Enable arm movement demo */
} exercise_session_config_t;

/**
 * @brief Initialize exercise follow app.
 *        Subscribes to EV_APP_MODE_CHANGED and exercise-related events.
 * @return esp_err_t
 */
esp_err_t exercise_follow_app_init(void);

/**
 * @brief Start exercise follow session with specified config.
 * @param config  Session configuration (NULL for defaults)
 * @return esp_err_t
 */
esp_err_t exercise_follow_app_start(const exercise_session_config_t *config);

/**
 * @brief Stop current exercise session.
 * @return esp_err_t
 */
esp_err_t exercise_follow_app_stop(void);

/**
 * @brief Pause current exercise session.
 * @return esp_err_t
 */
esp_err_t exercise_follow_app_pause(void);

/**
 * @brief Resume paused exercise session.
 * @return esp_err_t
 */
esp_err_t exercise_follow_app_resume(void);

/**
 * @brief Trigger a specific exercise action (called by vision module or touch).
 * @param action_id  Exercise action to perform
 * @return esp_err_t
 */
esp_err_t exercise_follow_app_trigger_action(exercise_action_id_t action_id);

/**
 * @brief Report user's movement match result (called by vision module).
 * @param action_id    The exercise action being matched
 * @param match_score  Match score (0-100), higher = better
 * @return esp_err_t
 */
esp_err_t exercise_follow_app_report_match(exercise_action_id_t action_id, uint8_t match_score);

#ifdef __cplusplus
}
#endif

#endif /* __EXERCISE_FOLLOW_APP_H__ */
