#include "motion_controller_config.h"

#if (MOTION_CONTROLLER_ENABLE == 1)

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "motion_controller.h"
#include "servo_driver.h"

static const char* TAG = "action_lib";

// Servo IDs
static uint8_t s_lx_ids[MOTION_SERVO_LX_COUNT] = MOTION_SERVO_LX_IDS;

// Action execution state (shared with motion_controller.c)
extern motion_state_t s_state;
extern volatile bool s_emergency_stop;
extern volatile bool s_stop_requested;
extern motion_action_status_t s_action_status;
extern const char* s_current_action;
extern motion_action_complete_cb_t s_action_cb;
extern TaskHandle_t s_action_task_handle;
extern motion_mode_t s_mode;
extern const motion_action_params_t s_default_params;

extern void set_state(motion_state_t new_state);
extern bool delay_with_stop_check(uint32_t ms);
extern int16_t motion_safety_clamp_position(uint8_t servo_id, int16_t position);
extern uint16_t motion_safety_clamp_speed(uint8_t servo_id, uint16_t speed);

// ==================== Action Keyframe Structure ====================

typedef struct
{
    int16_t em3_pos;
    int16_t lx_pos[MOTION_SERVO_LX_COUNT];
    uint16_t duration_ms;
} action_keyframe_t;

// ==================== Helper Functions ====================

static void move_to_keyframe(const action_keyframe_t* kf, uint16_t speed_factor)
{
    if (!kf) return;

    // Apply safety limits
    int16_t safe_em3 = motion_safety_clamp_position(MOTION_SERVO_EM3_ID, kf->em3_pos);
    int16_t safe_lx[MOTION_SERVO_LX_COUNT];
    for (int i = 0; i < MOTION_SERVO_LX_COUNT; i++)
    {
        safe_lx[i] = motion_safety_clamp_position(s_lx_ids[i], kf->lx_pos[i]);
    }

    uint16_t em3_speed = motion_safety_clamp_speed(MOTION_SERVO_EM3_ID, speed_factor);
    uint16_t lx_time = kf->duration_ms;

    servo_em3_move(MOTION_SERVO_EM3_ID, safe_em3, em3_speed);
    servo_lx_move_group(s_lx_ids, safe_lx, MOTION_SERVO_LX_COUNT, lx_time);
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

// Wave/Greeting action
static void execute_wave(const motion_action_params_t* params)
{
    int intensity = params ? params->intensity : 50;
    int repeat = params ? params->repeat_count : 1;

    move_to_home();

    for (int r = 0; r < repeat && !s_emergency_stop && !s_stop_requested; r++)
    {
        // Wave motion - LX5 (top joint) swings
        int16_t wave_range = 200 * intensity / 100;

        action_keyframe_t wave_right = {
            .em3_pos = MOTION_HOME_EM3_POS,
            .lx_pos = {MOTION_HOME_LX1_POS, MOTION_HOME_LX2_POS + 50, MOTION_HOME_LX3_POS, MOTION_HOME_LX5_POS + wave_range},
            .duration_ms = 400
        };
        move_to_keyframe(&wave_right, MOTION_EM3_MAX_SPEED / 2);
        if (!delay_with_stop_check(500)) return;

        action_keyframe_t wave_left = {
            .em3_pos = MOTION_HOME_EM3_POS,
            .lx_pos = {MOTION_HOME_LX1_POS, MOTION_HOME_LX2_POS - 50, MOTION_HOME_LX3_POS, MOTION_HOME_LX5_POS - wave_range},
            .duration_ms = 400
        };
        move_to_keyframe(&wave_left, MOTION_EM3_MAX_SPEED / 2);
        if (!delay_with_stop_check(500)) return;
    }

    move_to_home();
}

// Nod action
static void execute_nod(const motion_action_params_t* params)
{
    int intensity = params ? params->intensity : 50;
    int repeat = params ? params->repeat_count : 2;

    move_to_home();

    for (int r = 0; r < repeat && !s_emergency_stop && !s_stop_requested; r++)
    {
        int16_t nod_range = 150 * intensity / 100;

        // Nod down
        action_keyframe_t nod_down = {
            .em3_pos = MOTION_HOME_EM3_POS + nod_range,
            .lx_pos = {MOTION_HOME_LX1_POS, MOTION_HOME_LX2_POS + nod_range/2, MOTION_HOME_LX3_POS + nod_range/2, MOTION_HOME_LX5_POS},
            .duration_ms = 300
        };
        move_to_keyframe(&nod_down, MOTION_EM3_MAX_SPEED / 2);
        if (!delay_with_stop_check(400)) return;

        // Nod up
        action_keyframe_t nod_up = {
            .em3_pos = MOTION_HOME_EM3_POS - nod_range/2,
            .lx_pos = {MOTION_HOME_LX1_POS, MOTION_HOME_LX2_POS - nod_range/4, MOTION_HOME_LX3_POS - nod_range/4, MOTION_HOME_LX5_POS},
            .duration_ms = 300
        };
        move_to_keyframe(&nod_up, MOTION_EM3_MAX_SPEED / 2);
        if (!delay_with_stop_check(400)) return;
    }

    move_to_home();
}

// Shake head action
static void execute_shake_head(const motion_action_params_t* params)
{
    int intensity = params ? params->intensity : 50;
    int repeat = params ? params->repeat_count : 2;

    move_to_home();

    for (int r = 0; r < repeat && !s_emergency_stop && !s_stop_requested; r++)
    {
        int16_t shake_range = 300 * intensity / 100;

        // Shake left
        action_keyframe_t shake_left = {
            .em3_pos = MOTION_HOME_EM3_POS,
            .lx_pos = {MOTION_HOME_LX1_POS - shake_range, MOTION_HOME_LX2_POS, MOTION_HOME_LX3_POS, MOTION_HOME_LX5_POS},
            .duration_ms = 400
        };
        move_to_keyframe(&shake_left, MOTION_EM3_MAX_SPEED / 2);
        if (!delay_with_stop_check(500)) return;

        // Shake right
        action_keyframe_t shake_right = {
            .em3_pos = MOTION_HOME_EM3_POS,
            .lx_pos = {MOTION_HOME_LX1_POS + shake_range, MOTION_HOME_LX2_POS, MOTION_HOME_LX3_POS, MOTION_HOME_LX5_POS},
            .duration_ms = 400
        };
        move_to_keyframe(&shake_right, MOTION_EM3_MAX_SPEED / 2);
        if (!delay_with_stop_check(500)) return;
    }

    move_to_home();
}

// Come here action
static void execute_come_here(const motion_action_params_t* params)
{
    int intensity = params ? params->intensity : 50;

    // Save current pose as interaction-before pose
    motion_pose_save(POSE_SLOT_INTERACTION_BEFORE);

    // Move forward toward user
    int16_t forward_range = 400 * intensity / 100;

    action_keyframe_t forward = {
        .em3_pos = MOTION_HOME_EM3_POS + forward_range,
        .lx_pos = {MOTION_HOME_LX1_POS, MOTION_HOME_LX2_POS + forward_range/2, MOTION_HOME_LX3_POS + forward_range/3, MOTION_HOME_LX5_POS + forward_range/4},
        .duration_ms = 600
    };
    move_to_keyframe(&forward, MOTION_EM3_MAX_SPEED / 3);
    delay_with_stop_check(700);

    // Small greeting wave
    execute_wave(params);
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

// Breath/Company action (light movement)
static void execute_breath(const motion_action_params_t* params)
{
    int intensity = params ? params->intensity : 30;  // Default low intensity
    int repeat = params ? params->repeat_count : 5;

    for (int r = 0; r < repeat && !s_emergency_stop && !s_stop_requested; r++)
    {
        int16_t breath_range = 50 * intensity / 100;

        // Gentle breathing motion
        action_keyframe_t breath_up = {
            .em3_pos = MOTION_HOME_EM3_POS + breath_range,
            .lx_pos = {MOTION_HOME_LX1_POS, MOTION_HOME_LX2_POS + breath_range/2, MOTION_HOME_LX3_POS, MOTION_HOME_LX5_POS},
            .duration_ms = 1000
        };
        move_to_keyframe(&breath_up, MOTION_EM3_MAX_SPEED / 4);
        if (!delay_with_stop_check(1200)) return;

        action_keyframe_t breath_down = {
            .em3_pos = MOTION_HOME_EM3_POS - breath_range,
            .lx_pos = {MOTION_HOME_LX1_POS, MOTION_HOME_LX2_POS - breath_range/2, MOTION_HOME_LX3_POS, MOTION_HOME_LX5_POS},
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
    {"come_here", execute_come_here},
    {"go_back", NULL},  // Handled by pose_restore
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

static void action_task(void* arg)
{
    char action_name[32] = {0};
    motion_action_params_t params = s_default_params;

    if (arg)
    {
        // Parse argument (format: "action_name:params")
        char* colon = strchr((char*)arg, ':');
        if (colon)
        {
            strncpy(action_name, (char*)arg, colon - (char*)arg);
            // Parse params if needed (simplified for now)
        }
        else
        {
            strncpy(action_name, (char*)arg, sizeof(action_name) - 1);
        }
        free(arg);
    }

    ESP_LOGI(TAG, "Action task started: %s", action_name);

    s_current_action = action_name;
    s_action_status = ACTION_STATUS_RUNNING;
    set_state(MOTION_STATE_EXECUTING);

    // Apply mode to params
    params.mode = s_mode;

    // Find and execute action
    action_func_t func = find_action_func(action_name);

    if (func)
    {
        func(&params);

        if (s_emergency_stop || s_stop_requested)
        {
            s_action_status = ACTION_STATUS_INTERRUPTED;
            ESP_LOGI(TAG, "Action interrupted: %s", action_name);
        }
        else
        {
            s_action_status = ACTION_STATUS_COMPLETED;
            ESP_LOGI(TAG, "Action completed: %s", action_name);
        }
    }
    else
    {
        // Special actions
        if (strcmp(action_name, "home") == 0 || strcmp(action_name, "init") == 0)
        {
            move_to_home();
            s_action_status = ACTION_STATUS_COMPLETED;
        }
        else
        {
            ESP_LOGW(TAG, "Unknown action: %s", action_name);
            s_action_status = ACTION_STATUS_FAILED;
        }
    }

    s_current_action = NULL;
    s_action_task_handle = NULL;

    if (s_state == MOTION_STATE_EXECUTING)
    {
        set_state(MOTION_STATE_READY);
    }

    // Call completion callback
    if (s_action_cb)
    {
        s_action_cb(action_name, s_action_status);
    }

    vTaskDelete(NULL);
}

// ==================== Public Interface ====================

esp_err_t motion_action_execute(const char* action_name, const motion_action_params_t* params)
{
    if (!action_name)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_state == MOTION_STATE_UNINIT)
    {
        ESP_LOGW(TAG, "Motion controller not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_state == MOTION_STATE_EXECUTING || s_action_task_handle != NULL)
    {
        ESP_LOGW(TAG, "Action already executing");
        return ESP_ERR_INVALID_STATE;
    }

    // Special handling for go_back
    if (strcmp(action_name, "go_back") == 0)
    {
        return motion_pose_restore(POSE_SLOT_INTERACTION_BEFORE);
    }

    // Create argument string
    char* arg = strdup(action_name);
    if (!arg)
    {
        return ESP_ERR_NO_MEM;
    }

    // Execute blocking (simplified - runs task and waits)
    BaseType_t ret = xTaskCreate(action_task, "motion_action", MOTION_TASK_STACK_SIZE,
                                  arg, MOTION_TASK_PRIORITY, &s_action_task_handle);

    if (ret != pdPASS)
    {
        free(arg);
        ESP_LOGE(TAG, "Failed to create action task");
        return ESP_ERR_NO_MEM;
    }

    // Wait for completion (blocking)
    while (s_action_task_handle != NULL)
    {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    return ESP_OK;
}

esp_err_t motion_action_execute_async(const char* action_name, const motion_action_params_t* params)
{
    if (!action_name)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_state == MOTION_STATE_UNINIT)
    {
        ESP_LOGW(TAG, "Motion controller not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_state == MOTION_STATE_EXECUTING || s_action_task_handle != NULL)
    {
        ESP_LOGW(TAG, "Action already executing");
        return ESP_ERR_INVALID_STATE;
    }

    // Special handling for go_back
    if (strcmp(action_name, "go_back") == 0)
    {
        return motion_pose_restore(POSE_SLOT_INTERACTION_BEFORE);
    }

    // Create argument string
    char* arg = strdup(action_name);
    if (!arg)
    {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ret = xTaskCreate(action_task, "motion_action", MOTION_TASK_STACK_SIZE,
                                  arg, MOTION_TASK_PRIORITY, &s_action_task_handle);

    if (ret != pdPASS)
    {
        free(arg);
        ESP_LOGE(TAG, "Failed to create action task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t motion_action_stop(void)
{
    if (s_action_task_handle == NULL)
    {
        ESP_LOGW(TAG, "No action running");
        return ESP_ERR_INVALID_STATE;
    }

    s_stop_requested = true;
    ESP_LOGI(TAG, "Action stop requested");

    return ESP_OK;
}

motion_action_status_t motion_action_get_status(void)
{
    return s_action_status;
}

esp_err_t motion_set_action_callback(motion_action_complete_cb_t cb)
{
    s_action_cb = cb;
    return ESP_OK;
}

#endif // MOTION_CONTROLLER_ENABLE