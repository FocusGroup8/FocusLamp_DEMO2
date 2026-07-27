/*
 * exercise_follow_app.c - Exercise follow mode implementation
 * 
 * Coordinates arm demos, voice guidance, LED/LCD feedback for exercise breaks.
 * Vision module integration: calls exercise_follow_app_report_match() when
 * user movement is recognized.
 */

#include "exercise_follow_app.h"
#include "event_bus.h"
#include "event_def.h"
#include "led_service.h"
#include "lcd_service.h"
#include "audio_service.h"
#include "servo_service.h"
#include "arm_service.h"
#include "data_type.h"
#include "device_state.h"
#include "error_code.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "exercise_follow_app";

/* ===================== Default Session Config ===================== */
#define DEFAULT_TOTAL_DURATION_SEC   300  /* 5 min */
#define DEFAULT_WORK_DURATION_SEC    120  /* 2 min work before break */
#define DEFAULT_INTERVAL_COUNT       2    /* 2 exercise breaks */
#define DEFAULT_VOICE_GUIDE          true
#define DEFAULT_ARM_DEMO             true

/* ===================== Private Data ===================== */
static bool s_initialized = false;
static bool s_running = false;
static bool s_paused = false;

static esp_timer_handle_t s_exercise_timer = NULL;
static exercise_session_config_t s_config;
static uint32_t s_elapsed_sec = 0;
static exercise_action_id_t s_current_action = EXERCISE_ACTION_MAX;
static uint8_t s_completed_actions = 0;
static uint8_t s_match_score_accum = 0;
static uint8_t s_match_count = 0;

/* ===================== Name Helpers ===================== */
static const char *exercise_action_name(exercise_action_id_t id)
{
    static const char *names[] = {
        "Stretch Arms",
        "Stretch Neck",
        "Relax Shoulders",
        "Twist Waist",
        "Eye Relax",
        "Deep Breath",
    };
    if (id >= EXERCISE_ACTION_MAX) return "Unknown";
    return names[id];
}

static const char *exercise_action_tts_text(exercise_action_id_t id)
{
    static const char *texts[] = {
        "Let's stretch your arms. Follow me.",
        "Now stretch your neck gently.",
        "Relax your shoulders. Up and down.",
        "Twist your waist to the left and right.",
        "Close your eyes and take a rest.",
        "Take a deep breath in and out.",
    };
    if (id >= EXERCISE_ACTION_MAX) return "Great job!";
    return texts[id];
}

/* ===================== Action Sequence Builders ===================== */
static const action_sequence_t *exercise_get_arm_demo(exercise_action_id_t id)
{
    static action_step_t s_stretch_arm_steps[] = {
        { 0, 700, 500, 500 },  /* Right arm up */
        { 0, 300, 500, 500 },  /* Right arm down */
        { 0, 700, 500, 500 },  /* Repeat */
        { 0, 300, 500, 500 },
    };
    static const action_sequence_t s_stretch_arm_seq = {
        .steps = s_stretch_arm_steps, .step_count = 4, .loop_count = 2,
    };

    static action_step_t s_neck_steps[] = {
        { 1, 400, 600, 1000 },  /* Tilt forward */
        { 1, 600, 600, 1000 },  /* Tilt back */
    };
    static const action_sequence_t s_neck_seq = {
        .steps = s_neck_steps, .step_count = 2, .loop_count = 3,
    };

    static action_step_t s_shoulder_steps[] = {
        { 2, 400, 600, 500 },  /* Shoulder up */
        { 2, 600, 600, 500 },  /* Shoulder down */
    };
    static const action_sequence_t s_shoulder_seq = {
        .steps = s_shoulder_steps, .step_count = 2, .loop_count = 3,
    };

    static action_step_t s_waist_steps[] = {
        { 0, 600, 800, 800 },  /* Twist right */
        { 0, 400, 800, 800 },  /* Twist left */
        { 0, 600, 800, 800 },
        { 0, 400, 800, 800 },
    };
    static const action_sequence_t s_waist_seq = {
        .steps = s_waist_steps, .step_count = 4, .loop_count = 2,
    };

    /* For eye relax and deep breath, use gentle nodding rhythm */
    static action_step_t s_eye_relax_steps[] = {
        { 1, 450, 1000, 2000 },  /* Gentle nod */
        { 1, 550, 1000, 2000 },
    };
    static const action_sequence_t s_eye_relax_seq = {
        .steps = s_eye_relax_steps, .step_count = 2, .loop_count = 2,
    };

    static action_step_t s_breath_steps[] = {
        { 3, 450, 2000, 2000 },  /* Slow breath motion */
        { 3, 550, 2000, 2000 },
    };
    static const action_sequence_t s_breath_seq = {
        .steps = s_breath_steps, .step_count = 2, .loop_count = 3,
    };

    switch (id) {
        case EXERCISE_ACTION_STRETCH_ARM:    return &s_stretch_arm_seq;
        case EXERCISE_ACTION_STRETCH_NECK:   return &s_neck_seq;
        case EXERCISE_ACTION_SHOULDER_RELAX: return &s_shoulder_seq;
        case EXERCISE_ACTION_TWIST_WAIST:    return &s_waist_seq;
        case EXERCISE_ACTION_EYE_RELAX:      return &s_eye_relax_seq;
        case EXERCISE_ACTION_DEEP_BREATH:    return &s_breath_seq;
        default:                             return NULL;
    }
}

/* ===================== Execute Single Exercise Action ===================== */
static esp_err_t execute_exercise_action(exercise_action_id_t action_id)
{
    if (action_id >= EXERCISE_ACTION_MAX) {
        return ERR_INVALID_PARAM;
    }

    s_current_action = action_id;
    const char *name = exercise_action_name(action_id);
    ESP_LOGI(TAG, "Starting exercise: %s", name);

    /* Update LCD */
    lcd_service_info_set_title(name);
    lcd_service_page_switch_to(LCD_PAGE_INFO);
    lcd_service_info_update();

    /* LED: green breathing for exercise */
    led_service_set_mode(LED_MODE_COLOR);
    led_service_set_color(0, 255, 100);
    led_service_set_effect(LED_EFFECT_BREATHING);

    /* Voice guidance */
    if (s_config.voice_guide_enable) {
        audio_service_play_tts(exercise_action_tts_text(action_id));
    }

    /* Arm demo */
    if (s_config.arm_demo_enable) {
        const action_sequence_t *seq = exercise_get_arm_demo(action_id);
        if (seq != NULL) {
            arm_service_load_action(seq);
            arm_service_start_action();
        }
    }

    return ESP_OK;
}

/* ===================== Finish Current Action ===================== */
static void finish_current_action(void)
{
    if (s_current_action < EXERCISE_ACTION_MAX) {
        ESP_LOGI(TAG, "Exercise completed: %s", exercise_action_name(s_current_action));
        s_completed_actions++;
    }

    /* Stop arm action */
    arm_service_stop_action();

    s_current_action = EXERCISE_ACTION_MAX;

    /* Publish state change so UI can update */
    event_bus_publish_simple(EV_APP_STATE_CHANGED);
}

/* ===================== Timer Callback ===================== */
static void exercise_timer_callback(void *arg)
{
    (void)arg;

    if (!s_running || s_paused) {
        return;
    }

    s_elapsed_sec++;

    /* Check if we need to start an exercise break */
    if (s_config.work_duration_sec > 0 &&
        s_elapsed_sec % s_config.work_duration_sec == 0 &&
        s_current_action == EXERCISE_ACTION_MAX)
    {
        uint8_t interval = (uint8_t)(s_elapsed_sec / s_config.work_duration_sec);
        if (interval <= s_config.interval_count) {
            /* Pick an action based on interval index */
            exercise_action_id_t action_id = (exercise_action_id_t)(
                (interval - 1) % (uint8_t)EXERCISE_ACTION_MAX
            );
            execute_exercise_action(action_id);
        }
    }

    /* Check total duration */
    if (s_elapsed_sec >= s_config.total_duration_sec) {
        /* Session complete */
        ESP_LOGI(TAG, "Exercise session complete");
        audio_service_play_alert();
        lcd_service_info_set_title("Exercise Done!");
        lcd_service_info_update();
        exercise_follow_app_stop();
    }
}

/* ===================== Event Handlers ===================== */
static void exercise_app_on_mode_changed(event_t *event, void *context)
{
    (void)context;
    if (event == NULL || event->data == NULL || event->data_size < sizeof(uint8_t) * 2) {
        return;
    }
    const uint8_t *state_data = (const uint8_t *)event->data;
    app_state_t new_state = (app_state_t)state_data[1];
    if (new_state != APP_STATE_EXERCISE_FOLLOW && s_running) {
        exercise_follow_app_stop();
    }
}

static void exercise_app_on_touch_event(event_t *event, void *context)
{
    (void)context;
    if (event == NULL || !s_running) {
        return;
    }

    /* Touch A = skip/finish current exercise */
    if (event->type == EV_TOUCH_A_SINGLE_CLICK && s_current_action < EXERCISE_ACTION_MAX) {
        finish_current_action();
        audio_service_play_alert();
        lcd_service_info_set_title("Skipped");
        lcd_service_info_update();
    }

    /* Touch B = pause/resume */
    if (event->type == EV_TOUCH_B_SINGLE_CLICK) {
        if (s_paused) {
            exercise_follow_app_resume();
        } else {
            exercise_follow_app_pause();
        }
    }
}

/* ===================== Public API ===================== */
esp_err_t exercise_follow_app_init(void)
{
    if (s_initialized) {
        return ERR_ALREADY_INITIALIZED;
    }

    esp_err_t ret = event_bus_subscribe(EV_APP_MODE_CHANGED, exercise_app_on_mode_changed, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to EV_APP_MODE_CHANGED");
        return ret;
    }

    ret = event_bus_subscribe(EV_TOUCH_A_SINGLE_CLICK, exercise_app_on_touch_event, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to EV_TOUCH_A_SINGLE_CLICK");
        return ret;
    }

    ret = event_bus_subscribe(EV_TOUCH_B_SINGLE_CLICK, exercise_app_on_touch_event, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to EV_TOUCH_B_SINGLE_CLICK");
        return ret;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = exercise_timer_callback,
        .arg = NULL,
        .name = "exercise_timer",
        .dispatch_method = ESP_TIMER_TASK,
    };
    ret = esp_timer_create(&timer_args, &s_exercise_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create exercise timer");
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Exercise follow app initialized");
    return ESP_OK;
}

esp_err_t exercise_follow_app_start(const exercise_session_config_t *config)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    if (s_running) {
        return ERR_BUSY;
    }

    /* Apply config or defaults */
    if (config != NULL) {
        memcpy(&s_config, config, sizeof(s_config));
    } else {
        s_config.total_duration_sec = DEFAULT_TOTAL_DURATION_SEC;
        s_config.work_duration_sec = DEFAULT_WORK_DURATION_SEC;
        s_config.interval_count = DEFAULT_INTERVAL_COUNT;
        s_config.voice_guide_enable = DEFAULT_VOICE_GUIDE;
        s_config.arm_demo_enable = DEFAULT_ARM_DEMO;
    }

    s_elapsed_sec = 0;
    s_current_action = EXERCISE_ACTION_MAX;
    s_completed_actions = 0;
    s_match_score_accum = 0;
    s_match_count = 0;
    s_paused = false;
    s_running = true;

    /* Apply lighting */
    led_service_set_mode(LED_MODE_COLOR);
    led_service_set_color(0, 180, 80);
    led_service_set_effect(LED_EFFECT_STEADY);
    led_service_set_brightness(200);

    /* LCD */
    lcd_service_info_set_title("Exercise Ready");
    lcd_service_page_switch_to(LCD_PAGE_INFO);
    lcd_service_info_update();

    /* Voice intro */
    if (s_config.voice_guide_enable) {
        audio_service_play_tts("Exercise time! I will guide you through the movements.");
    }

    /* Start timer (1 second period) */
    esp_timer_start_periodic(s_exercise_timer, 1000000);

    event_bus_publish_simple(EV_APP_STATE_CHANGED);
    ESP_LOGI(TAG, "Exercise session started: %lu sec total, %lu sec intervals",
             (unsigned long)s_config.total_duration_sec,
             (unsigned long)s_config.work_duration_sec);

    return ESP_OK;
}

esp_err_t exercise_follow_app_stop(void)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    if (!s_running) {
        return ERR_BUSY;
    }

    if (s_exercise_timer != NULL) {
        esp_timer_stop(s_exercise_timer);
    }

    /* Stop arm and restore */
    arm_service_stop_action();
    led_service_turn_off();
    lcd_service_page_switch_to(LCD_PAGE_EXPRESSION);
    lcd_service_expression_set(LCD_EXPRESSION_NORMAL);

    s_running = false;
    s_paused = false;
    s_current_action = EXERCISE_ACTION_MAX;
    s_elapsed_sec = 0;

    event_bus_publish_simple(EV_APP_STATE_CHANGED);
    ESP_LOGI(TAG, "Exercise session stopped, %u actions completed", s_completed_actions);
    return ESP_OK;
}

esp_err_t exercise_follow_app_pause(void)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    if (!s_running || s_paused) {
        return ERR_BUSY;
    }

    if (s_exercise_timer != NULL) {
        esp_timer_stop(s_exercise_timer);
    }

    arm_service_stop_action();

    s_paused = true;
    lcd_service_info_set_title("Paused");
    lcd_service_info_update();
    led_service_set_effect(LED_EFFECT_BLINKING);

    event_bus_publish_simple(EV_APP_STATE_CHANGED);
    ESP_LOGI(TAG, "Exercise session paused");
    return ESP_OK;
}

esp_err_t exercise_follow_app_resume(void)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    if (!s_running || !s_paused) {
        return ERR_BUSY;
    }

    if (s_exercise_timer != NULL) {
        esp_timer_start_periodic(s_exercise_timer, 1000000);
    }

    s_paused = false;
    lcd_service_info_set_title("Resumed");
    lcd_service_info_update();
    led_service_set_effect(LED_EFFECT_STEADY);

    event_bus_publish_simple(EV_APP_STATE_CHANGED);
    ESP_LOGI(TAG, "Exercise session resumed");
    return ESP_OK;
}

esp_err_t exercise_follow_app_trigger_action(exercise_action_id_t action_id)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    if (!s_running) {
        return ERR_BUSY;
    }
    if (action_id >= EXERCISE_ACTION_MAX) {
        return ERR_INVALID_PARAM;
    }

    /* If already performing an action, skip new trigger */
    if (s_current_action < EXERCISE_ACTION_MAX) {
        ESP_LOGW(TAG, "Exercise already in progress: %s", exercise_action_name(s_current_action));
        return ERR_BUSY;
    }

    return execute_exercise_action(action_id);
}

esp_err_t exercise_follow_app_report_match(exercise_action_id_t action_id, uint8_t match_score)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    if (action_id >= EXERCISE_ACTION_MAX) {
        return ERR_INVALID_PARAM;
    }

    s_match_score_accum += match_score;
    s_match_count++;

    char title[32];
    snprintf(title, sizeof(title), "%s: %u%%", exercise_action_name(action_id), match_score);
    lcd_service_info_set_title(title);
    lcd_service_info_update();

    ESP_LOGI(TAG, "Match report: %s score=%u%%",
             exercise_action_name(action_id), match_score);

    /* Provide audio encouragement */
    if (match_score >= 80 && s_config.voice_guide_enable) {
        audio_service_play_tts("Excellent! Keep it up!");
    } else if (match_score < 40 && s_config.voice_guide_enable) {
        audio_service_play_tts("Try to follow along. Like this.");
    }

    /* Mark this action as done after match report */
    if (action_id == s_current_action) {
        finish_current_action();
    }

    return ESP_OK;
}
