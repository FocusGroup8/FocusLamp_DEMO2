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

// Nod action - LX5 (top joint) ±200, 3 times
static void execute_nod(const motion_action_params_t* params)
{
    (void)params;
    int repeat = 3;

    move_to_home();

    for (int r = 0; r < repeat && !s_emergency_stop && !s_stop_requested; r++)
    {
        action_keyframe_t nod_down = {
            .em3_pos = MOTION_HOME_EM3_POS,
            .lx_pos = {MOTION_HOME_LX1_POS, MOTION_HOME_LX2_POS, MOTION_HOME_LX3_POS, MOTION_HOME_LX5_POS + 200},
            .duration_ms = 300
        };
        move_to_keyframe(&nod_down, MOTION_EM3_MAX_SPEED / 2);
        if (!delay_with_stop_check(400)) return;

        action_keyframe_t nod_up = {
            .em3_pos = MOTION_HOME_EM3_POS,
            .lx_pos = {MOTION_HOME_LX1_POS, MOTION_HOME_LX2_POS, MOTION_HOME_LX3_POS, MOTION_HOME_LX5_POS - 200},
            .duration_ms = 300
        };
        move_to_keyframe(&nod_up, MOTION_EM3_MAX_SPEED / 2);
        if (!delay_with_stop_check(400)) return;
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

// Happy emotion action
static void execute_happy(const motion_action_params_t* params)
{
    move_to_home();

    // Happy: bounce + wave
    int intensity = params ? params->intensity : 70;

    for (int i = 0; i < 2 && !s_emergency_stop && !s_stop_requested; i++)
    {
        int16_t happy_range = 200 * intensity / 100;

        action_keyframe_t happy_up = {
            .em3_pos = MOTION_HOME_EM3_POS + happy_range,
            .lx_pos = {MOTION_HOME_LX1_POS, MOTION_HOME_LX2_POS - happy_range/2, MOTION_HOME_LX3_POS - happy_range/3, MOTION_HOME_LX5_POS},
            .duration_ms = 300
        };
        move_to_keyframe(&happy_up, MOTION_EM3_MAX_SPEED / 2);
        if (!delay_with_stop_check(400)) return;

        action_keyframe_t happy_down = {
            .em3_pos = MOTION_HOME_EM3_POS - happy_range/2,
            .lx_pos = {MOTION_HOME_LX1_POS, MOTION_HOME_LX2_POS + happy_range/2, MOTION_HOME_LX3_POS + happy_range/3, MOTION_HOME_LX5_POS},
            .duration_ms = 300
        };
        move_to_keyframe(&happy_down, MOTION_EM3_MAX_SPEED / 2);
        if (!delay_with_stop_check(400)) return;
    }

    execute_wave(params);
}

// Curious emotion action
static void execute_curious(const motion_action_params_t* params)
{
    move_to_home();

    int intensity = params ? params->intensity : 50;
    int16_t curious_range = 150 * intensity / 100;

    // Lean forward and look around
    action_keyframe_t curious_forward = {
        .em3_pos = MOTION_HOME_EM3_POS + curious_range,
        .lx_pos = {MOTION_HOME_LX1_POS, MOTION_HOME_LX2_POS + curious_range/2, MOTION_HOME_LX3_POS + curious_range/3, MOTION_HOME_LX5_POS},
        .duration_ms = 500
    };
    move_to_keyframe(&curious_forward, MOTION_EM3_MAX_SPEED / 3);
    if (!delay_with_stop_check(600)) return;

    // Look left
    action_keyframe_t curious_left = {
        .em3_pos = MOTION_HOME_EM3_POS + curious_range,
        .lx_pos = {MOTION_HOME_LX1_POS - curious_range, MOTION_HOME_LX2_POS + curious_range/2, MOTION_HOME_LX3_POS, MOTION_HOME_LX5_POS},
        .duration_ms = 400
    };
    move_to_keyframe(&curious_left, MOTION_EM3_MAX_SPEED / 2);
    if (!delay_with_stop_check(500)) return;

    // Look right
    action_keyframe_t curious_right = {
        .em3_pos = MOTION_HOME_EM3_POS + curious_range,
        .lx_pos = {MOTION_HOME_LX1_POS + curious_range, MOTION_HOME_LX2_POS + curious_range/2, MOTION_HOME_LX3_POS, MOTION_HOME_LX5_POS},
        .duration_ms = 400
    };
    move_to_keyframe(&curious_right, MOTION_EM3_MAX_SPEED / 2);
    if (!delay_with_stop_check(500)) return;

    move_to_home();
}

// Sleepy emotion action
static void execute_sleepy(const motion_action_params_t* params)
{
    int intensity = params ? params->intensity : 40;
    int16_t sleepy_range = 100 * intensity / 100;

    // Slow drooping motion
    action_keyframe_t sleepy_down = {
        .em3_pos = MOTION_HOME_EM3_POS - sleepy_range,
        .lx_pos = {MOTION_HOME_LX1_POS, MOTION_HOME_LX2_POS + sleepy_range, MOTION_HOME_LX3_POS + sleepy_range/2, MOTION_HOME_LX5_POS + sleepy_range/3},
        .duration_ms = 2000
    };
    move_to_keyframe(&sleepy_down, MOTION_EM3_MAX_SPEED / 6);
    delay_with_stop_check(2500);

    // Slow rise
    action_keyframe_t sleepy_up = {
        .em3_pos = MOTION_HOME_EM3_POS + sleepy_range/2,
        .lx_pos = {MOTION_HOME_LX1_POS, MOTION_HOME_LX2_POS - sleepy_range/2, MOTION_HOME_LX3_POS - sleepy_range/4, MOTION_HOME_LX5_POS},
        .duration_ms = 1500
    };
    move_to_keyframe(&sleepy_up, MOTION_EM3_MAX_SPEED / 6);
    delay_with_stop_check(2000);

    // Droop again
    move_to_keyframe(&sleepy_down, MOTION_EM3_MAX_SPEED / 6);
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
    {"happy", execute_happy},
    {"curious", execute_curious},
    {"sleepy", execute_sleepy},
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
    return name && find_action_func(name) != NULL;
}

void motion_library_run(const char *name, const motion_action_params_t *params)
{
    action_func_t fn = find_action_func(name);
    if (fn) fn(params);
}

#endif /* MOTION_CONTROLLER_ENABLE */
