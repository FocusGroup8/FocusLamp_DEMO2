/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "algo_result_manager.h"

#include "base_bridge.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "tts_inject.h"

#if CONFIG_EXAMPLE_DEMO_EXPRESSIVE_EYES
#include "expressive_eyes_display.h"
#endif

#include <string.h>

static const char *TAG = "algo_mgr";

/*---------------------------------------------------------------
 * Constants
 *-------------------------------------------------------------*/
/* VLM phone/computer detection: forward to base board + TTS to voice board.
 * Throttled to >=60s per source; a source change forwards immediately. */
#define VLM_FORWARD_THROTTLE_US (60LL * 1000LL * 1000LL) /* 60s */

/* Gesture thumb up/down: forward to base board arm.
 * Throttled to >=1s, only on gesture change. */
#define GESTURE_FORWARD_THROTTLE_US (1LL * 1000LL * 1000LL) /* 1s */

/* TTS cooldown for sedentary reminder */
#define SEDENTARY_TTS_COOLDOWN_US (60 * 1000000LL) /* 60s */

/* Presence thresholds */
#define SEDENTARY_THRESHOLD_S 60 /* 60s presence triggers sedentary TTS */
#define PRESENCE_FALSE_RESET_S 5 /* 5s absence resets sedentary timer */

/*---------------------------------------------------------------
 * Static state
 *-------------------------------------------------------------*/
static algo_mode_t s_current_mode      = ALGO_MODE_NORMAL;
static char s_last_vlm_source[16]      = {0};
static int64_t s_last_vlm_forward_us   = 0;
static char s_last_gesture[32]         = {0};
static int64_t s_last_gesture_us       = 0;
static float s_sedentary_timer_s       = 0.0f;
static int64_t s_last_sedentary_tts_ts = 0;
static int64_t s_presence_false_ts     = 0;
static bool s_initialized              = false;

/*---------------------------------------------------------------
 * Internal: Map emotion string to expressive_eyes expression
 *-------------------------------------------------------------*/
#if CONFIG_EXAMPLE_DEMO_EXPRESSIVE_EYES
static expressive_eyes_expression_t emotion_to_expression(const char *emotion)
{
    if (!emotion) {
        return EXPRESSIVE_EYES_NEUTRAL;
    }
    if (strcmp(emotion, "happiness") == 0) {
        return EXPRESSIVE_EYES_HAPPY;
    }
    if (strcmp(emotion, "sadness") == 0) {
        return EXPRESSIVE_EYES_SAD;
    }
    if (strcmp(emotion, "anger") == 0) {
        return EXPRESSIVE_EYES_ANGRY;
    }
    if (strcmp(emotion, "surprise") == 0) {
        return EXPRESSIVE_EYES_SURPRISED;
    }
    /* contempt/disgust/fear → BORED (simplified mapping, per project convention) */
    if (strcmp(emotion, "contempt") == 0 || strcmp(emotion, "disgust") == 0 || strcmp(emotion, "fear") == 0) {
        return EXPRESSIVE_EYES_BORED;
    }
    /* neutral and unknown default to NEUTRAL */
    return EXPRESSIVE_EYES_NEUTRAL;
}
#endif

/*---------------------------------------------------------------
 * Sub-handler: Emotion → eyes expression (all modes)
 *-------------------------------------------------------------*/
static void handle_emotion(const cJSON *emotion_item)
{
    if (!cJSON_IsString(emotion_item) || !emotion_item->valuestring) {
        return;
    }
    const char *emotion = emotion_item->valuestring;

#if CONFIG_EXAMPLE_DEMO_EXPRESSIVE_EYES
    expressive_eyes_expression_t expr = emotion_to_expression(emotion);
    expressive_eyes_set_expression(expr);
    ESP_LOGD(TAG, "emotion=%s -> eyes=%d", emotion, (int)expr);
#else
    ESP_LOGD(TAG, "emotion=%s (expressive_eyes disabled)", emotion);
#endif

    /* Companion mode: force happy expression */
    if (s_current_mode == ALGO_MODE_COMPANION) {
#if CONFIG_EXAMPLE_DEMO_EXPRESSIVE_EYES
        expressive_eyes_set_expression(EXPRESSIVE_EYES_HAPPY);
#endif
    }
}

/*---------------------------------------------------------------
 * Sub-handler: Focus → small screen display (all modes)
 * TODO: connect to small screen display area (deferred to ESP display team)
 *-------------------------------------------------------------*/
static void handle_focus(const cJSON *focus_item)
{
    if (!focus_item) {
        return;
    }
    const cJSON *engage = cJSON_GetObjectItem(focus_item, "engage_level_name");
    const cJSON *level  = cJSON_GetObjectItem(focus_item, "focus_level_name");
    const cJSON *score  = cJSON_GetObjectItem(focus_item, "focus_score");

    ESP_LOGD(TAG, "focus: engage=%s level=%s score=%.2f",
             engage ? (cJSON_IsString(engage) ? engage->valuestring : "?") : "?",
             level ? (cJSON_IsString(level) ? level->valuestring : "?") : "?",
             score ? (cJSON_IsNumber(score) ? score->valuedouble : 0.0) : 0.0);
}

/*---------------------------------------------------------------
 * Sub-handler: VLM game → forward to base board + TTS (Normal/Focus modes)
 *
 * Trigger: judgment=="是" AND trigger_source contains 手机/电脑.
 * Actions:
 *   1. base_bridge_post_detect_phone(source) → base board (arm response)
 *   2. tts_inject_speak(message) → voice board (user reminder)
 * Throttle: same source >=60s; source change forwards immediately.
 *-------------------------------------------------------------*/
static void handle_vlm_game(const cJSON *vlm_item)
{
    if (!vlm_item) {
        return;
    }
    const cJSON *judgment = cJSON_GetObjectItem(vlm_item, "judgment");
    if (!cJSON_IsString(judgment) || !judgment->valuestring) {
        return;
    }
    /* VLM detector outputs Chinese: 是/否/不确定/等待检测 */
    if (strcmp(judgment->valuestring, "是") != 0) {
        return;
    }

    const cJSON *trigger_source = cJSON_GetObjectItem(vlm_item, "trigger_source");
    if (!cJSON_IsString(trigger_source) || !trigger_source->valuestring) {
        return;
    }

    const char *source  = NULL;
    const char *tts_msg = NULL;
    if (strstr(trigger_source->valuestring, "手机")) {
        source  = "phone";
        tts_msg = "检测到您正在玩手机，请注意专注";
    } else if (strstr(trigger_source->valuestring, "电脑")) {
        source  = "computer";
        tts_msg = "检测到您正在使用电脑，请注意专注";
    }
    if (!source) {
        return;
    }

    int64_t now_us        = esp_timer_get_time();
    bool source_changed   = (strcmp(s_last_vlm_source, source) != 0);
    bool throttle_elapsed = (now_us - s_last_vlm_forward_us) >= VLM_FORWARD_THROTTLE_US;

    if (source_changed || throttle_elapsed) {
        /* Forward detection to base board (failure logged, doesn't block TTS) */
        esp_err_t err = base_bridge_post_detect_phone(source);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "VLM forward to base failed: source=%s", source);
        }

        /* TTS reminder to voice board */
        if (tts_msg) {
            tts_inject_speak(tts_msg);
        }

        /* Update throttle state (prevents TTS spam even if HTTP failed) */
        snprintf(s_last_vlm_source, sizeof(s_last_vlm_source), "%s", source);
        s_last_vlm_forward_us = now_us;
        ESP_LOGI(TAG, "vlm_game: %s detected -> base_bridge + TTS forwarded", source);
    } else {
        ESP_LOGD(TAG, "vlm_game: %s throttled", source);
    }
}

/*---------------------------------------------------------------
 * Sub-handler: Presence → sedentary timer + TTS (Normal/Focus modes)
 *-------------------------------------------------------------*/
static void handle_presence(const cJSON *presence_item)
{
    if (!presence_item) {
        return;
    }
    const cJSON *present  = cJSON_GetObjectItem(presence_item, "present");
    const cJSON *duration = cJSON_GetObjectItem(presence_item, "duration_s");

    if (!cJSON_IsBool(present)) {
        return;
    }

    bool is_present = cJSON_IsTrue(present);
    double dur_s    = cJSON_IsNumber(duration) ? duration->valuedouble : 0.0;

    if (is_present) {
        s_presence_false_ts = 0;
        s_sedentary_timer_s = (float)dur_s;

        if (s_sedentary_timer_s >= SEDENTARY_THRESHOLD_S) {
            int64_t now = esp_timer_get_time();
            if (now - s_last_sedentary_tts_ts >= SEDENTARY_TTS_COOLDOWN_US) {
                s_last_sedentary_tts_ts = now;
                tts_inject_speak("您已久坐超过一分钟，建议起身活动");
                ESP_LOGI(TAG, "presence: sedentary %.0fs -> TTS forwarded", s_sedentary_timer_s);
            }
        }
    } else {
        /* Track absence duration; reset timer after PRESENCE_FALSE_RESET_S */
        if (s_presence_false_ts == 0) {
            s_presence_false_ts = esp_timer_get_time();
        }
        int64_t now = esp_timer_get_time();
        if (now - s_presence_false_ts > PRESENCE_FALSE_RESET_S * 1000000LL) {
            s_sedentary_timer_s = 0.0f;
            ESP_LOGD(TAG, "presence: absent >%ds, timer reset", PRESENCE_FALSE_RESET_S);
        }
    }
}

/*---------------------------------------------------------------
 * Sub-handler: Gesture → arm control (Companion mode only)
 *-------------------------------------------------------------*/
static void handle_gesture(const cJSON *gesture_item)
{
    if (!cJSON_IsString(gesture_item) || !gesture_item->valuestring) {
        return;
    }
    const char *gesture = gesture_item->valuestring;

    const char *arm_action = NULL;
    if (strcmp(gesture, "Thumb_Up") == 0) {
        arm_action = "thumb_up";
    } else if (strcmp(gesture, "Thumb_Down") == 0) {
        arm_action = "thumb_down";
    }
    if (!arm_action) {
        return;
    }

    int64_t now_us        = esp_timer_get_time();
    bool gesture_changed  = (strcmp(s_last_gesture, arm_action) != 0);
    bool throttle_elapsed = (now_us - s_last_gesture_us) >= GESTURE_FORWARD_THROTTLE_US;

    if (gesture_changed && throttle_elapsed) {
        esp_err_t err = base_bridge_post_arm_gesture(arm_action);
        if (err == ESP_OK) {
            snprintf(s_last_gesture, sizeof(s_last_gesture), "%s", arm_action);
            s_last_gesture_us = now_us;
            ESP_LOGI(TAG, "gesture: %s -> arm %s forwarded", gesture, arm_action);
        } else {
            ESP_LOGW(TAG, "gesture forward failed: %s", arm_action);
        }
    }
}

/*---------------------------------------------------------------
 * Public: Handle algorithm result arguments
 *-------------------------------------------------------------*/
esp_err_t algo_result_handle(const cJSON *arguments)
{
    if (!s_initialized || !arguments) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 1. handle_emotion - all modes */
    handle_emotion(cJSON_GetObjectItem(arguments, "emotion"));

    /* 2. handle_focus - all modes */
    handle_focus(cJSON_GetObjectItem(arguments, "focus"));

    /* 3. handle_vlm_game - Normal/Focus only (suppressed in Companion) */
    if (s_current_mode != ALGO_MODE_COMPANION) {
        handle_vlm_game(cJSON_GetObjectItem(arguments, "vlm_game_detector"));
    }

    /* 4. handle_presence - Normal/Focus only (suppressed in Companion) */
    if (s_current_mode != ALGO_MODE_COMPANION) {
        handle_presence(cJSON_GetObjectItem(arguments, "presence"));
    }

    /* 5. handle_gesture - Companion only (suppressed in Normal/Focus) */
    if (s_current_mode == ALGO_MODE_COMPANION) {
        handle_gesture(cJSON_GetObjectItem(arguments, "gesture"));
    }

    return ESP_OK;
}

/*---------------------------------------------------------------
 * Public: Mode state machine
 *-------------------------------------------------------------*/
void algo_result_set_mode(algo_mode_t mode)
{
    if (mode == s_current_mode) {
        return;
    }
    ESP_LOGI(TAG, "mode change: %d -> %d, resetting timers/cooldowns", s_current_mode, mode);
    s_current_mode = mode;

    /* Reset all timers and cooldowns on mode switch */
    s_sedentary_timer_s     = 0.0f;
    s_last_vlm_forward_us   = 0;
    s_last_vlm_source[0]    = '\0';
    s_last_gesture_us       = 0;
    s_last_gesture[0]       = '\0';
    s_last_sedentary_tts_ts = 0;
    s_presence_false_ts     = 0;

    /* Focus mode: 30min countdown would start here (spec §3.7) */
    if (mode == ALGO_MODE_FOCUS) {
        ESP_LOGI(TAG, "Focus mode entered, 30min countdown started");
    }
}

/*---------------------------------------------------------------
 * Public: Initialize
 *-------------------------------------------------------------*/
esp_err_t algo_result_manager_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    s_current_mode          = ALGO_MODE_NORMAL;
    s_last_vlm_source[0]    = '\0';
    s_last_vlm_forward_us   = 0;
    s_last_gesture[0]       = '\0';
    s_last_gesture_us       = 0;
    s_sedentary_timer_s     = 0.0f;
    s_last_sedentary_tts_ts = 0;
    s_presence_false_ts     = 0;
    s_initialized           = true;
    ESP_LOGI(TAG, "Algo result manager initialized (HTTP bridge mode)");
    return ESP_OK;
}
