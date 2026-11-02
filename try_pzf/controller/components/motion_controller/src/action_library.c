#include "motion_controller_config.h"

#if (MOTION_CONTROLLER_ENABLE == 1)

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "motion_controller.h"
#include "servo_driver.h"



// Servo IDs


/* Shared cancellation flags are atomic; all bus commands have one owner. */
#include <stdatomic.h>
extern atomic_bool s_stop_requested;
extern atomic_bool s_emergency_stop;
extern bool delay_with_stop_check(uint32_t ms);

// ==================== Action Keyframe Structure ====================

typedef struct
{
    int16_t em3_pos;
    int16_t lx_pos[MOTION_SERVO_LX_COUNT];
    uint16_t duration_ms;
} action_keyframe_t;

// ==================== Helper Functions ====================

static void move_to_keyframe(const action_keyframe_t *kf, uint16_t speed_factor)
{
    if (!kf) return;
    int16_t pos[5] = {kf->em3_pos, kf->lx_pos[0], kf->lx_pos[1], kf->lx_pos[2], kf->lx_pos[3]};
    motion_send_pose(pos, kf->duration_ms, speed_factor);
}

static void move_to_home(void)
{
    action_keyframe_t home_kf = {
        .em3_pos = MOTION_HOME_EM3_POS,
        .lx_pos = {MOTION_HOME_LX1_POS, MOTION_HOME_LX2_POS, MOTION_HOME_LX3_POS, MOTION_HOME_LX5_POS},
        .duration_ms = 800
    };
    move_to_keyframe(&home_kf, MOTION_EM3_MAX_SPEED / 2);
    delay_with_stop_check(800);
}

// ==================== Action Definitions ====================

// Wave action - LX1 (base rotation) ±150, 3 times
static void execute_wave(const motion_action_params_t* params)
{
    (void)params;
    int repeat = 3;

    move_to_home();

    for (int r = 0; r < repeat && !s_emergency_stop && !s_stop_requested; r++)
    {
        action_keyframe_t wave_right = {
            .em3_pos = MOTION_HOME_EM3_POS,
            .lx_pos = {MOTION_HOME_LX1_POS + 150, MOTION_HOME_LX2_POS, MOTION_HOME_LX3_POS, MOTION_HOME_LX5_POS},
            .duration_ms = 400
        };
        move_to_keyframe(&wave_right, MOTION_EM3_MAX_SPEED / 2);
        if (!delay_with_stop_check(500)) return;

        action_keyframe_t wave_left = {
            .em3_pos = MOTION_HOME_EM3_POS,
            .lx_pos = {MOTION_HOME_LX1_POS - 150, MOTION_HOME_LX2_POS, MOTION_HOME_LX3_POS, MOTION_HOME_LX5_POS},
            .duration_ms = 400
        };
        move_to_keyframe(&wave_left, MOTION_EM3_MAX_SPEED / 2);
        if (!delay_with_stop_check(500)) return;
    }

    move_to_home();
}

// Nod action - LX5 (top joint) ±150, 3 times
static void execute_nod(const motion_action_params_t* params)
{
    (void)params;
    int repeat = 3;

    move_to_home();

    for (int r = 0; r < repeat && !s_emergency_stop && !s_stop_requested; r++)
    {
        action_keyframe_t nod_down = {
            .em3_pos = MOTION_HOME_EM3_POS,
            .lx_pos = {MOTION_HOME_LX1_POS, MOTION_HOME_LX2_POS, MOTION_HOME_LX3_POS, MOTION_HOME_LX5_POS + 150},
            .duration_ms = 500
        };
        move_to_keyframe(&nod_down, MOTION_EM3_MAX_SPEED / 2);
        if (!delay_with_stop_check(600)) return;

        action_keyframe_t nod_up = {
            .em3_pos = MOTION_HOME_EM3_POS,
            .lx_pos = {MOTION_HOME_LX1_POS, MOTION_HOME_LX2_POS, MOTION_HOME_LX3_POS, MOTION_HOME_LX5_POS - 150},
            .duration_ms = 500
        };
        move_to_keyframe(&nod_up, MOTION_EM3_MAX_SPEED / 2);
        if (!delay_with_stop_check(600)) return;
    }

    move_to_home();
}

// Shake head action - EM3 ±350, 3 times
static void execute_shake_head(const motion_action_params_t* params)
{
    (void)params;
    int repeat = 3;

    move_to_home();

    for (int r = 0; r < repeat && !s_emergency_stop && !s_stop_requested; r++)
    {
        action_keyframe_t shake_left = {
            .em3_pos = MOTION_HOME_EM3_POS + 350,
            .lx_pos = {MOTION_HOME_LX1_POS, MOTION_HOME_LX2_POS, MOTION_HOME_LX3_POS, MOTION_HOME_LX5_POS},
            .duration_ms = 400
        };
        move_to_keyframe(&shake_left, MOTION_EM3_MAX_SPEED / 2);
        if (!delay_with_stop_check(500)) return;

        action_keyframe_t shake_right = {
            .em3_pos = MOTION_HOME_EM3_POS - 350,
            .lx_pos = {MOTION_HOME_LX1_POS, MOTION_HOME_LX2_POS, MOTION_HOME_LX3_POS, MOTION_HOME_LX5_POS},
            .duration_ms = 400
        };
        move_to_keyframe(&shake_right, MOTION_EM3_MAX_SPEED / 2);
        if (!delay_with_stop_check(500)) return;
    }

    move_to_home();
}

// Dance action
static void execute_dance(const motion_action_params_t* params)
{
    int intensity = params ? params->intensity : 50;
    int repeat = params ? params->repeat_count : 3;

    move_to_home();

    for (int r = 0; r < repeat && !s_emergency_stop && !s_stop_requested; r++)
    {
        int16_t dance_range = 300 * intensity / 100;

        // Dance move 1: sway left
        action_keyframe_t sway_left = {
            .em3_pos = MOTION_HOME_EM3_POS + dance_range/2,
            .lx_pos = {MOTION_HOME_LX1_POS - dance_range, MOTION_HOME_LX2_POS + dance_range/2, MOTION_HOME_LX3_POS, MOTION_HOME_LX5_POS + dance_range/3},
            .duration_ms = 500
        };
        move_to_keyframe(&sway_left, MOTION_EM3_MAX_SPEED / 2);
        if (!delay_with_stop_check(600)) return;

        // Dance move 2: sway right
        action_keyframe_t sway_right = {
            .em3_pos = MOTION_HOME_EM3_POS - dance_range/2,
            .lx_pos = {MOTION_HOME_LX1_POS + dance_range, MOTION_HOME_LX2_POS - dance_range/2, MOTION_HOME_LX3_POS, MOTION_HOME_LX5_POS - dance_range/3},
            .duration_ms = 500
        };
        move_to_keyframe(&sway_right, MOTION_EM3_MAX_SPEED / 2);
        if (!delay_with_stop_check(600)) return;

        // Dance move 3: bounce
        action_keyframe_t bounce = {
            .em3_pos = MOTION_HOME_EM3_POS + dance_range,
            .lx_pos = {MOTION_HOME_LX1_POS, MOTION_HOME_LX2_POS + dance_range/2, MOTION_HOME_LX3_POS + dance_range/2, MOTION_HOME_LX5_POS},
            .duration_ms = 300
        };
        move_to_keyframe(&bounce, MOTION_EM3_MAX_SPEED / 2);
        if (!delay_with_stop_check(400)) return;
    }

    move_to_home();
}

// Breath/Company action - LX3 ±50 + LX5 ±50, 3 times
static void execute_breath(const motion_action_params_t* params)
{
    (void)params;
    int repeat = 3;

    for (int r = 0; r < repeat && !s_emergency_stop && !s_stop_requested; r++)
    {
        action_keyframe_t breath_up = {
            .em3_pos = MOTION_HOME_EM3_POS,
            .lx_pos = {MOTION_HOME_LX1_POS, MOTION_HOME_LX2_POS, MOTION_HOME_LX3_POS + 50, MOTION_HOME_LX5_POS + 50},
            .duration_ms = 1000
        };
        move_to_keyframe(&breath_up, MOTION_EM3_MAX_SPEED / 4);
        if (!delay_with_stop_check(1200)) return;

        action_keyframe_t breath_down = {
            .em3_pos = MOTION_HOME_EM3_POS,
            .lx_pos = {MOTION_HOME_LX1_POS, MOTION_HOME_LX2_POS, MOTION_HOME_LX3_POS - 50, MOTION_HOME_LX5_POS - 50},
            .duration_ms = 1000
        };
        move_to_keyframe(&breath_down, MOTION_EM3_MAX_SPEED / 4);
        if (!delay_with_stop_check(1200)) return;
    }
}

// Focus action (very light movement for focus mode)
static void execute_focus(const motion_action_params_t* params)
{
    int intensity = params ? params->intensity : 20;  // Very low intensity
    int repeat = params ? params->repeat_count : 3;

    for (int r = 0; r < repeat && !s_emergency_stop && !s_stop_requested; r++)
    {
        int16_t focus_range = 30 * intensity / 100;

        // Minimal movement
        action_keyframe_t focus_move = {
            .em3_pos = MOTION_HOME_EM3_POS + focus_range,
            .lx_pos = {MOTION_HOME_LX1_POS, MOTION_HOME_LX2_POS + focus_range/2, MOTION_HOME_LX3_POS, MOTION_HOME_LX5_POS},
            .duration_ms = 2000
        };
        move_to_keyframe(&focus_move, MOTION_EM3_MAX_SPEED / 6);
        if (!delay_with_stop_check(2500)) return;

        // Return
        action_keyframe_t focus_return = {
            .em3_pos = MOTION_HOME_EM3_POS,
            .lx_pos = {MOTION_HOME_LX1_POS, MOTION_HOME_LX2_POS, MOTION_HOME_LX3_POS, MOTION_HOME_LX5_POS},
            .duration_ms = 2000
        };
        move_to_keyframe(&focus_return, MOTION_EM3_MAX_SPEED / 6);
        if (!delay_with_stop_check(2500)) return;
    }
}

// Greet/Welcome action
static void execute_greet(const motion_action_params_t* params)
{
    move_to_home();

    // Welcome gesture: nod + wave
    execute_nod(params);
    if (!s_emergency_stop && !s_stop_requested)
    {
        execute_wave(params);
    }
}

// Exercise action (simple stretching)
static void execute_exercise(const motion_action_params_t* params)
{
    int intensity = params ? params->intensity : 50;
    int repeat = params ? params->repeat_count : 2;

    move_to_home();

    for (int r = 0; r < repeat && !s_emergency_stop && !s_stop_requested; r++)
    {
        int16_t stretch_range = 200 * intensity / 100;

        // Stretch up
        action_keyframe_t stretch_up = {
            .em3_pos = MOTION_HOME_EM3_POS + stretch_range,
            .lx_pos = {MOTION_HOME_LX1_POS, MOTION_HOME_LX2_POS - stretch_range, MOTION_HOME_LX3_POS - stretch_range/2, MOTION_HOME_LX5_POS - stretch_range/3},
            .duration_ms = 800
        };
        move_to_keyframe(&stretch_up, MOTION_EM3_MAX_SPEED / 3);
        if (!delay_with_stop_check(1000)) return;

        // Stretch down
        action_keyframe_t stretch_down = {
            .em3_pos = MOTION_HOME_EM3_POS - stretch_range/2,
            .lx_pos = {MOTION_HOME_LX1_POS, MOTION_HOME_LX2_POS + stretch_range, MOTION_HOME_LX3_POS + stretch_range/2, MOTION_HOME_LX5_POS + stretch_range/3},
            .duration_ms = 800
        };
        move_to_keyframe(&stretch_down, MOTION_EM3_MAX_SPEED / 3);
        if (!delay_with_stop_check(1000)) return;

        // Rotate
        action_keyframe_t rotate = {
            .em3_pos = MOTION_HOME_EM3_POS,
            .lx_pos = {MOTION_HOME_LX1_POS + stretch_range, MOTION_HOME_LX2_POS, MOTION_HOME_LX3_POS, MOTION_HOME_LX5_POS},
            .duration_ms = 600
        };
        move_to_keyframe(&rotate, MOTION_EM3_MAX_SPEED / 2);
        if (!delay_with_stop_check(700)) return;

        action_keyframe_t rotate_back = {
            .em3_pos = MOTION_HOME_EM3_POS,
            .lx_pos = {MOTION_HOME_LX1_POS - stretch_range, MOTION_HOME_LX2_POS, MOTION_HOME_LX3_POS, MOTION_HOME_LX5_POS},
            .duration_ms = 600
        };
        move_to_keyframe(&rotate_back, MOTION_EM3_MAX_SPEED / 2);
        if (!delay_with_stop_check(700)) return;
    }

    move_to_home();
}

// Emotion plans share the existing worker, cancellation and bus ownership.
#include "motion_emotion_plan.h"

static esp_err_t execute_emotion(const motion_emotion_plan_t *plan,
    const motion_action_params_t *params, const int16_t start[5])
{
    int intensity=motion_emotion_intensity(plan->name,params ? params->intensity : 50);
    int minimum[5], maximum[5];
    int16_t target[5];
    const int ids[5]={4,1,2,3,5};
    for (int axis=0; axis<5; ++axis) {
        motion_manual_servo_limits_t limits;
        esp_err_t ret=motion_manual_servo_get_limits(ids[axis],&limits);
        if (ret!=ESP_OK || !limits.configured) return ESP_ERR_NOT_SUPPORTED;
        minimum[axis]=limits.min_position;
        maximum[axis]=limits.max_position;
    }
    for (size_t i=0; i<plan->count; ++i)
        if (!motion_emotion_target(start,&plan->steps[i],intensity,minimum,maximum,target))
            return ESP_ERR_INVALID_ARG;

    int16_t previous_em3=start[0];
    for (size_t i=0; i<plan->count; ++i) {
        if (s_emergency_stop || s_stop_requested) break;
        const motion_emotion_step_t *step=&plan->steps[i];
        motion_emotion_target(start,step,intensity,minimum,maximum,target);
        unsigned difference=abs((int)target[0]-previous_em3);
        unsigned speed=(difference*1000u+step->duration_ms-1u)/step->duration_ms;
        if (speed<1) speed=1;
        if (speed>100) speed=100;
        motion_send_pose(target,step->duration_ms,(uint16_t)speed);
        if (!delay_with_stop_check(step->duration_ms+step->hold_ms)) break;
        previous_em3=target[0];
    }
    return ESP_OK; // The worker reports cancellation and verifies final feedback.
}

// ==================== Action Task ====================

typedef void (*action_func_t)(const motion_action_params_t*);

typedef struct
{
    const char* name;
    action_func_t func;
} action_entry_t;

static const action_entry_t s_action_table[] = {
    {"wave", execute_wave},
    {"nod", execute_nod},
    {"shake_head", execute_shake_head},
    {"shake", execute_shake_head},  // Alias
    {"dance", execute_dance},
    {"greet", execute_greet},
    {"welcome", execute_greet},  // Alias
    {"breath", execute_breath},
    {"company", execute_breath},  // Alias
    {"focus", execute_focus},
    {"exercise", execute_exercise},
    {"home", NULL},  // Handled by home manager
    {"init", NULL},  // Handled by home manager
};

static action_func_t find_action_func(const char* name)
{
    for (int i = 0; i < sizeof(s_action_table) / sizeof(s_action_table[0]); i++)
    {
        if (strcmp(name, s_action_table[i].name) == 0)
        {
            return s_action_table[i].func;
        }
    }
    return NULL;
}


bool motion_library_has_action(const char *name)
{
    return name && (motion_emotion_find(name) || find_action_func(name) != NULL);
}

esp_err_t motion_library_run(const char *name, const motion_action_params_t *params,
                             const int16_t start[5])
{
    const motion_emotion_plan_t *plan=motion_emotion_find(name);
    if (plan) return execute_emotion(plan,params,start);
    action_func_t fn = find_action_func(name);
    if (!fn) return ESP_ERR_NOT_SUPPORTED;
    fn(params);
    return ESP_OK;
}

#endif /* MOTION_CONTROLLER_ENABLE */
