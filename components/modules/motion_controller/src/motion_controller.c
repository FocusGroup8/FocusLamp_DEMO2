#include "motion_controller_config.h"

#if (MOTION_CONTROLLER_ENABLE == 1)

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "motion_controller.h"
#include "servo_driver.h"

#if (EVENT_BUS_ENABLE == 1)
#include "event_bus.h"
#endif

static const char* TAG = "motion_ctrl";

// ==================== Error Value Detection ====================

#define SERVO_POS_ERROR_VALUE (-32768)
#define SERVO_POS_IS_ERROR(pos) ((pos) <= SERVO_POS_ERROR_VALUE + 100)

// ==================== Last Valid Position Cache ====================

static int16_t s_last_valid_em3_pos = MOTION_HOME_EM3_POS;
static int16_t s_last_valid_lx_pos[MOTION_SERVO_LX_COUNT] = {
    MOTION_HOME_LX1_POS, MOTION_HOME_LX2_POS, MOTION_HOME_LX3_POS, MOTION_HOME_LX5_POS
};

// ==================== Internal State ====================

// Internal state (non-static: shared with action_library.c)
motion_state_t s_state = MOTION_STATE_UNINIT;
motion_mode_t s_mode = MOTION_MODE_NORMAL;
static bool s_initialized = false;
static bool s_homing_completed = false;
static TaskHandle_t s_homing_task_handle = NULL;
TaskHandle_t s_action_task_handle = NULL;
volatile bool s_emergency_stop = false;
volatile bool s_stop_requested = false;
static SemaphoreHandle_t s_mutex = NULL;

// Pose storage
static motion_pose_t s_saved_poses[POSE_SLOT_MAX] = {0};

// Action state (non-static: shared with action_library.c)
motion_action_status_t s_action_status = ACTION_STATUS_IDLE;
const char* s_current_action = NULL;
static motion_interrupt_cb_t s_interrupt_cb = NULL;
motion_action_complete_cb_t s_action_cb = NULL;

// Servo IDs
static uint8_t s_lx_ids[MOTION_SERVO_LX_COUNT] = MOTION_SERVO_LX_IDS;

// Default action parameters (non-static: shared with action_library.c)
const motion_action_params_t s_default_params = {
    .intensity = 50,
    .speed = 50,
    .repeat_count = 1,
    .mode = MOTION_MODE_NORMAL
};

// ==================== Safety Limits Table ====================

typedef struct {
    int16_t em3_min_pos;
    int16_t em3_max_pos;
    int16_t lx_min_pos;
    int16_t lx_max_pos;
    uint16_t em3_max_speed;
    uint16_t lx_max_speed;
    uint16_t em3_max_accel;
    uint16_t lx_max_accel;
} mode_limits_t;

static const mode_limits_t s_mode_limits[] = {
    // MOTION_MODE_NORMAL: full range
    {
        .em3_min_pos = MOTION_EM3_MIN_POS,
        .em3_max_pos = MOTION_EM3_MAX_POS,
        .lx_min_pos = MOTION_LX_MIN_POS,
        .lx_max_pos = MOTION_LX_MAX_POS,
        .em3_max_speed = MOTION_EM3_MAX_SPEED,
        .lx_max_speed = MOTION_LX_MAX_SPEED,
        .em3_max_accel = 100,
        .lx_max_accel = 100,
    },
    // MOTION_MODE_FOCUS: limited range/speed
    {
        .em3_min_pos = (int16_t)(MOTION_HOME_EM3_POS - (MOTION_HOME_EM3_POS - MOTION_EM3_MIN_POS) * MOTION_FOCUS_AMPLITUDE_RATIO / 100),
        .em3_max_pos = (int16_t)(MOTION_HOME_EM3_POS + (MOTION_EM3_MAX_POS - MOTION_HOME_EM3_POS) * MOTION_FOCUS_AMPLITUDE_RATIO / 100),
        .lx_min_pos = (int16_t)(MOTION_HOME_LX1_POS - (MOTION_HOME_LX1_POS - MOTION_LX_MIN_POS) * MOTION_FOCUS_AMPLITUDE_RATIO / 100),
        .lx_max_pos = (int16_t)(MOTION_HOME_LX1_POS + (MOTION_LX_MAX_POS - MOTION_HOME_LX1_POS) * MOTION_FOCUS_AMPLITUDE_RATIO / 100),
        .em3_max_speed = MOTION_EM3_MAX_SPEED * MOTION_FOCUS_SPEED_RATIO / 100,
        .lx_max_speed = MOTION_LX_MAX_SPEED * MOTION_FOCUS_SPEED_RATIO / 100,
        .em3_max_accel = 100 * MOTION_FOCUS_SPEED_RATIO / 100,
        .lx_max_accel = 100 * MOTION_FOCUS_SPEED_RATIO / 100,
    },
    // MOTION_MODE_COMPANY: medium range/speed
    {
        .em3_min_pos = (int16_t)(MOTION_HOME_EM3_POS - (MOTION_HOME_EM3_POS - MOTION_EM3_MIN_POS) * MOTION_COMPANY_AMPLITUDE_RATIO / 100),
        .em3_max_pos = (int16_t)(MOTION_HOME_EM3_POS + (MOTION_EM3_MAX_POS - MOTION_HOME_EM3_POS) * MOTION_COMPANY_AMPLITUDE_RATIO / 100),
        .lx_min_pos = (int16_t)(MOTION_HOME_LX1_POS - (MOTION_HOME_LX1_POS - MOTION_LX_MIN_POS) * MOTION_COMPANY_AMPLITUDE_RATIO / 100),
        .lx_max_pos = (int16_t)(MOTION_HOME_LX1_POS + (MOTION_LX_MAX_POS - MOTION_HOME_LX1_POS) * MOTION_COMPANY_AMPLITUDE_RATIO / 100),
        .em3_max_speed = MOTION_EM3_MAX_SPEED * MOTION_COMPANY_SPEED_RATIO / 100,
        .lx_max_speed = MOTION_LX_MAX_SPEED * MOTION_COMPANY_SPEED_RATIO / 100,
        .em3_max_accel = 100 * MOTION_COMPANY_SPEED_RATIO / 100,
        .lx_max_accel = 100 * MOTION_COMPANY_SPEED_RATIO / 100,
    },
    // MOTION_MODE_SHOW: full range (same as normal)
    {
        .em3_min_pos = MOTION_EM3_MIN_POS,
        .em3_max_pos = MOTION_EM3_MAX_POS,
        .lx_min_pos = MOTION_LX_MIN_POS,
        .lx_max_pos = MOTION_LX_MAX_POS,
        .em3_max_speed = MOTION_EM3_MAX_SPEED,
        .lx_max_speed = MOTION_LX_MAX_SPEED,
        .em3_max_accel = 100,
        .lx_max_accel = 100,
    },
};

// ==================== Helper Functions (non-static: shared with action_library.c) ====================

void set_state(motion_state_t new_state)
{
    if (s_state == new_state) {
        return;
    }

    motion_state_t old_state = s_state;
    s_state = new_state;

    ESP_LOGI(TAG, "State changed: %d -> %d", old_state, new_state);

#if (EVENT_BUS_ENABLE == 1)
    // Publish state change event
    uint32_t state_data = ((uint32_t)old_state << 16) | (uint32_t)new_state;
    event_bus_publish_simple(EVENT_TYPE_SYSTEM_STATE, SYSTEM_EVENT_STATE_CHANGE,
                             &state_data, sizeof(state_data));
#endif

    // Call interrupt callback if registered
    if (s_interrupt_cb) {
        s_interrupt_cb(old_state, new_state);
    }
}

bool delay_with_stop_check(uint32_t ms)
{
    uint32_t elapsed = 0;
    const uint32_t check_interval_ms = 10;

    while (elapsed < ms) {
        if (s_emergency_stop || s_stop_requested) {
            return false;
        }

        uint32_t remaining = ms - elapsed;
        uint32_t sleep_ms = (remaining < check_interval_ms) ? remaining : check_interval_ms;

        vTaskDelay(pdMS_TO_TICKS(sleep_ms));
        elapsed += sleep_ms;
    }

    return !(s_emergency_stop || s_stop_requested);
}

void read_current_pose(motion_pose_t* pose)
{
    if (!pose) {
        return;
    }

    memset(pose, 0, sizeof(motion_pose_t));

    int16_t em3_pos = servo_em3_read_pos(MOTION_SERVO_EM3_ID);
    if (SERVO_POS_IS_ERROR(em3_pos)) {
        ESP_LOGW(TAG, "EM3 read failed (%d), using last valid position: %d", em3_pos, s_last_valid_em3_pos);
        pose->em3_pos = s_last_valid_em3_pos;
    } else {
        s_last_valid_em3_pos = em3_pos;  // Update cache
        pose->em3_pos = em3_pos;
    }

    for (int i = 0; i < MOTION_SERVO_LX_COUNT; i++) {
        int16_t lx_pos = servo_lx_read_pos(s_lx_ids[i]);
        if (SERVO_POS_IS_ERROR(lx_pos)) {
            ESP_LOGW(TAG, "LX ID%d read failed (%d), using last valid position: %d", 
                     s_lx_ids[i], lx_pos, s_last_valid_lx_pos[i]);
            pose->lx_pos[i] = s_last_valid_lx_pos[i];
        } else {
            s_last_valid_lx_pos[i] = lx_pos;  // Update cache
            pose->lx_pos[i] = lx_pos;
        }
    }

    pose->timestamp = (uint32_t)(esp_timer_get_time() / 1000);
    pose->valid = true;
}

// ==================== Home Manager ====================

static void execute_homing_sequence(void)
{
    ESP_LOGI(TAG, "Starting homing sequence...");

    set_state(MOTION_STATE_HOMING);

    // Step 0: Enable torque on all servos
    ESP_LOGI(TAG, "Enabling torque on EM3 (ID%d)", MOTION_SERVO_EM3_ID);
    servo_em3_enable_torque(MOTION_SERVO_EM3_ID, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    // Step 1: Move EM3 (ID4) to home position
    ESP_LOGI(TAG, "Homing EM3 (ID%d) -> %d", MOTION_SERVO_EM3_ID, MOTION_HOME_EM3_POS);
    servo_em3_move(MOTION_SERVO_EM3_ID, (uint16_t)MOTION_HOME_EM3_POS, MOTION_EM3_MAX_SPEED / 2);

    // Step 2: Move LX servos to home positions simultaneously
    int16_t lx_home_pos[MOTION_SERVO_LX_COUNT] = {
        MOTION_HOME_LX1_POS,
        MOTION_HOME_LX2_POS,
        MOTION_HOME_LX3_POS,
        MOTION_HOME_LX5_POS
    };

    ESP_LOGI(TAG, "Homing LX servos -> [%d, %d, %d, %d]",
             lx_home_pos[0], lx_home_pos[1], lx_home_pos[2], lx_home_pos[3]);
    servo_lx_move_group(s_lx_ids, lx_home_pos, MOTION_SERVO_LX_COUNT, MOTION_HOME_TIME_MS);

    // Wait for homing to complete
    if (!delay_with_stop_check(MOTION_HOME_TIME_MS + 500)) {
        ESP_LOGW(TAG, "Homing interrupted");
        return;
    }

    // Verify positions
    int16_t em3_actual = servo_em3_read_pos(MOTION_SERVO_EM3_ID);
    if (SERVO_POS_IS_ERROR(em3_actual)) {
        ESP_LOGW(TAG, "EM3 position read failed (error value: %d) - servo may not be connected", em3_actual);
    } else {
        ESP_LOGI(TAG, "EM3 position after homing: %d (target: %d)", em3_actual, MOTION_HOME_EM3_POS);
    }

    for (int i = 0; i < MOTION_SERVO_LX_COUNT; i++) {
        int16_t lx_actual = servo_lx_read_pos(s_lx_ids[i]);
        if (SERVO_POS_IS_ERROR(lx_actual)) {
            ESP_LOGW(TAG, "LX ID%d position read failed (error value: %d) - servo may not be connected", 
                     s_lx_ids[i], lx_actual);
        } else {
            ESP_LOGI(TAG, "LX ID%d position after homing: %d (target: %d)",
                     s_lx_ids[i], lx_actual, lx_home_pos[i]);
        }
    }

    s_homing_completed = true;
    ESP_LOGI(TAG, "Homing sequence completed");

    set_state(MOTION_STATE_READY);
}

esp_err_t motion_home_execute(void)
{
    if (s_state == MOTION_STATE_UNINIT) {
        ESP_LOGW(TAG, "Motion controller not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_state == MOTION_STATE_HOMING) {
        ESP_LOGW(TAG, "Homing already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_state == MOTION_STATE_EXECUTING) {
        ESP_LOGW(TAG, "Cannot home while action is executing");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_emergency_stop) {
        ESP_LOGW(TAG, "Cannot home in emergency stop state");
        return ESP_ERR_INVALID_STATE;
    }

    execute_homing_sequence();

    return s_homing_completed ? ESP_OK : ESP_FAIL;
}

static void homing_task(void* arg)
{
    execute_homing_sequence();

    s_homing_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t motion_home_async_execute(void)
{
    if (s_state == MOTION_STATE_UNINIT) {
        ESP_LOGW(TAG, "Motion controller not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_state == MOTION_STATE_HOMING) {
        ESP_LOGW(TAG, "Homing already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_state == MOTION_STATE_EXECUTING) {
        ESP_LOGW(TAG, "Cannot home while action is executing");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_emergency_stop) {
        ESP_LOGW(TAG, "Cannot home in emergency stop state");
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t ret = xTaskCreate(homing_task, "motion_home",
                                  MOTION_TASK_STACK_SIZE, NULL,
                                  MOTION_TASK_PRIORITY, &s_homing_task_handle);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create homing task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

bool motion_home_is_completed(void)
{
    return s_homing_completed;
}

// ==================== Startup Test ====================

esp_err_t motion_startup_test(void)
{
    if (s_state == MOTION_STATE_UNINIT) {
        ESP_LOGW(TAG, "Motion controller not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Executing startup test sequence...");

    set_state(MOTION_STATE_HOMING);

    // Step 1: Move all servos slightly (about 20 degrees offset from home)
    int16_t test_offset = 200;  // About 20 degrees for LX servos
    int16_t em3_test_offset = 100;  // About 10 degrees for EM3

    // Move EM3 to test position
    int16_t em3_test_pos = MOTION_HOME_EM3_POS + em3_test_offset;
    ESP_LOGI(TAG, "Test: Moving EM3 to %d", em3_test_pos);
    servo_em3_move(MOTION_SERVO_EM3_ID, (uint16_t)em3_test_pos, MOTION_EM3_MAX_SPEED / 3);

    // Move LX servos to test positions
    int16_t lx_test_pos[MOTION_SERVO_LX_COUNT] = {
        MOTION_HOME_LX1_POS + test_offset,
        MOTION_HOME_LX2_POS + test_offset,
        MOTION_HOME_LX3_POS - test_offset,  // Alternate direction
        MOTION_HOME_LX5_POS - test_offset
    };

    ESP_LOGI(TAG, "Test: Moving LX servos to [%d, %d, %d, %d]",
             lx_test_pos[0], lx_test_pos[1], lx_test_pos[2], lx_test_pos[3]);
    servo_lx_move_group(s_lx_ids, lx_test_pos, MOTION_SERVO_LX_COUNT, 800);

    // Wait for test movement to complete
    if (!delay_with_stop_check(1000)) {
        ESP_LOGW(TAG, "Startup test interrupted");
        set_state(MOTION_STATE_IDLE);
        return ESP_ERR_INVALID_STATE;
    }

    // Step 2: Return to home positions
    ESP_LOGI(TAG, "Test: Returning to home positions...");
    servo_em3_move(MOTION_SERVO_EM3_ID, (uint16_t)MOTION_HOME_EM3_POS, MOTION_EM3_MAX_SPEED / 2);

    int16_t lx_home_pos[MOTION_SERVO_LX_COUNT] = {
        MOTION_HOME_LX1_POS,
        MOTION_HOME_LX2_POS,
        MOTION_HOME_LX3_POS,
        MOTION_HOME_LX5_POS
    };
    servo_lx_move_group(s_lx_ids, lx_home_pos, MOTION_SERVO_LX_COUNT, MOTION_HOME_TIME_MS);

    // Wait for homing to complete
    if (!delay_with_stop_check(MOTION_HOME_TIME_MS + 500)) {
        ESP_LOGW(TAG, "Startup test interrupted during homing");
        set_state(MOTION_STATE_IDLE);
        return ESP_ERR_INVALID_STATE;
    }

    s_homing_completed = true;
    ESP_LOGI(TAG, "Startup test sequence completed");

    set_state(MOTION_STATE_READY);
    return ESP_OK;
}

// ==================== Pose Manager ====================

esp_err_t motion_pose_save(motion_pose_slot_t slot)
{
    if (slot < 0 || slot >= POSE_SLOT_MAX) {
        ESP_LOGW(TAG, "Invalid pose slot: %d", slot);
        return ESP_ERR_INVALID_ARG;
    }

    if (s_state == MOTION_STATE_UNINIT) {
        ESP_LOGW(TAG, "Motion controller not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for pose save");
        return ESP_ERR_TIMEOUT;
    }

    read_current_pose(&s_saved_poses[slot]);

    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Pose saved to slot %d: EM3=%d, LX=[%d,%d,%d,%d]",
             slot,
             s_saved_poses[slot].em3_pos,
             s_saved_poses[slot].lx_pos[0],
             s_saved_poses[slot].lx_pos[1],
             s_saved_poses[slot].lx_pos[2],
             s_saved_poses[slot].lx_pos[3]);

    return ESP_OK;
}

esp_err_t motion_pose_restore(motion_pose_slot_t slot)
{
    if (slot < 0 || slot >= POSE_SLOT_MAX) {
        ESP_LOGW(TAG, "Invalid pose slot: %d", slot);
        return ESP_ERR_INVALID_ARG;
    }

    if (s_state == MOTION_STATE_UNINIT) {
        ESP_LOGW(TAG, "Motion controller not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_saved_poses[slot].valid) {
        ESP_LOGW(TAG, "No valid pose in slot %d", slot);
        return ESP_ERR_NOT_FOUND;
    }

    if (s_emergency_stop) {
        ESP_LOGW(TAG, "Cannot restore pose in emergency stop state");
        return ESP_ERR_INVALID_STATE;
    }

    motion_pose_t pose;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for pose restore");
        return ESP_ERR_TIMEOUT;
    }

    memcpy(&pose, &s_saved_poses[slot], sizeof(motion_pose_t));
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Restoring pose from slot %d: EM3=%d, LX=[%d,%d,%d,%d]",
             slot, pose.em3_pos,
             pose.lx_pos[0], pose.lx_pos[1], pose.lx_pos[2], pose.lx_pos[3]);

    set_state(MOTION_STATE_EXECUTING);

    // Apply safety clamping before moving
    int16_t safe_em3 = motion_safety_clamp_position(MOTION_SERVO_EM3_ID, pose.em3_pos);
    int16_t safe_lx[MOTION_SERVO_LX_COUNT];
    for (int i = 0; i < MOTION_SERVO_LX_COUNT; i++) {
        safe_lx[i] = motion_safety_clamp_position(s_lx_ids[i], pose.lx_pos[i]);
    }

    uint16_t em3_speed = motion_safety_clamp_speed(MOTION_SERVO_EM3_ID, MOTION_EM3_MAX_SPEED / 2);
    uint16_t lx_time = MOTION_HOME_TIME_MS;

    servo_em3_move(MOTION_SERVO_EM3_ID, (uint16_t)safe_em3, em3_speed);
    servo_lx_move_group(s_lx_ids, safe_lx, MOTION_SERVO_LX_COUNT, lx_time);

    if (!delay_with_stop_check(lx_time + 300)) {
        ESP_LOGW(TAG, "Pose restore interrupted");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_state == MOTION_STATE_EXECUTING) {
        set_state(MOTION_STATE_READY);
    }

    return ESP_OK;
}

esp_err_t motion_pose_get(motion_pose_slot_t slot, motion_pose_t* pose)
{
    if (slot < 0 || slot >= POSE_SLOT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!pose) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    memcpy(pose, &s_saved_poses[slot], sizeof(motion_pose_t));
    xSemaphoreGive(s_mutex);

    return s_saved_poses[slot].valid ? ESP_OK : ESP_ERR_NOT_FOUND;
}

bool motion_pose_is_valid(motion_pose_slot_t slot)
{
    if (slot < 0 || slot >= POSE_SLOT_MAX) {
        return false;
    }

    return s_saved_poses[slot].valid;
}

// ==================== Safety Limiter ====================

static const mode_limits_t* get_current_limits(void)
{
    int idx = (int)s_mode;
    if (idx < 0 || idx >= sizeof(s_mode_limits) / sizeof(s_mode_limits[0])) {
        idx = 0;
    }
    return &s_mode_limits[idx];
}

bool motion_safety_check_position(uint8_t servo_id, int16_t position)
{
    const mode_limits_t* limits = get_current_limits();

    if (servo_id == MOTION_SERVO_EM3_ID) {
        return (position >= limits->em3_min_pos && position <= limits->em3_max_pos);
    } else {
        // LX servo
        return (position >= limits->lx_min_pos && position <= limits->lx_max_pos);
    }
}

int16_t motion_safety_clamp_position(uint8_t servo_id, int16_t position)
{
    const mode_limits_t* limits = get_current_limits();

    if (servo_id == MOTION_SERVO_EM3_ID) {
        if (position < limits->em3_min_pos) {
            ESP_LOGW(TAG, "EM3 pos clamped: %d -> %d (min)", position, limits->em3_min_pos);
            return limits->em3_min_pos;
        }
        if (position > limits->em3_max_pos) {
            ESP_LOGW(TAG, "EM3 pos clamped: %d -> %d (max)", position, limits->em3_max_pos);
            return limits->em3_max_pos;
        }
    } else {
        // LX servo
        if (position < limits->lx_min_pos) {
            ESP_LOGW(TAG, "LX ID%d pos clamped: %d -> %d (min)", servo_id, position, limits->lx_min_pos);
            return limits->lx_min_pos;
        }
        if (position > limits->lx_max_pos) {
            ESP_LOGW(TAG, "LX ID%d pos clamped: %d -> %d (max)", servo_id, position, limits->lx_max_pos);
            return limits->lx_max_pos;
        }
    }

    return position;
}

uint16_t motion_safety_clamp_speed(uint8_t servo_id, uint16_t speed)
{
    const mode_limits_t* limits = get_current_limits();

    if (servo_id == MOTION_SERVO_EM3_ID) {
        if (speed > limits->em3_max_speed) {
            ESP_LOGW(TAG, "EM3 speed clamped: %d -> %d", speed, limits->em3_max_speed);
            return limits->em3_max_speed;
        }
    } else {
        // LX servo
        if (speed > limits->lx_max_speed) {
            ESP_LOGW(TAG, "LX ID%d speed clamped: %d -> %d", servo_id, speed, limits->lx_max_speed);
            return limits->lx_max_speed;
        }
    }

    return speed;
}

esp_err_t motion_safety_get_limits(motion_safety_limits_t* limits)
{
    if (!limits) {
        return ESP_ERR_INVALID_ARG;
    }

    const mode_limits_t* current = get_current_limits();

    limits->em3_min_pos = current->em3_min_pos;
    limits->em3_max_pos = current->em3_max_pos;
    limits->lx_min_pos = current->lx_min_pos;
    limits->lx_max_pos = current->lx_max_pos;
    limits->em3_max_speed = current->em3_max_speed;
    limits->lx_max_speed = current->lx_max_speed;
    limits->em3_max_accel = current->em3_max_accel;
    limits->lx_max_accel = current->lx_max_accel;

    return ESP_OK;
}

esp_err_t motion_controller_set_mode(motion_mode_t mode)
{
    if (mode < 0 || mode > MOTION_MODE_SHOW) {
        ESP_LOGW(TAG, "Invalid motion mode: %d", mode);
        return ESP_ERR_INVALID_ARG;
    }

    if (mode == s_mode) {
        return ESP_OK;
    }

    motion_mode_t old_mode = s_mode;
    s_mode = mode;

    ESP_LOGI(TAG, "Mode changed: %d -> %d", old_mode, mode);

#if (EVENT_BUS_ENABLE == 1)
    // Publish mode change event
    typedef struct {
        uint8_t servo_id;
        uint8_t old_mode;
        uint8_t new_mode;
    } mode_change_data_t;

    mode_change_data_t data = {
        .servo_id = 0,
        .old_mode = (uint8_t)old_mode,
        .new_mode = (uint8_t)mode,
    };
    event_bus_publish_simple(EVENT_TYPE_SERVO_CONTROL, SERVO_EVENT_MODE_CHANGED,
                             &data, sizeof(data));
#endif

    return ESP_OK;
}

motion_mode_t motion_controller_get_mode(void)
{
    return s_mode;
}

// ==================== Interrupt Handler ====================

esp_err_t motion_emergency_stop(void)
{
    ESP_LOGW(TAG, "EMERGENCY STOP activated!");

    s_emergency_stop = true;
    s_stop_requested = true;

    // Disable torque on all servos immediately
    servo_em3_enable_torque(MOTION_SERVO_EM3_ID, 0);
    servo_lx_unload_all(s_lx_ids, MOTION_SERVO_LX_COUNT);

    set_state(MOTION_STATE_EMERGENCY_STOP);

    return ESP_OK;
}

esp_err_t motion_soft_stop(void)
{
    ESP_LOGI(TAG, "Soft stop requested");

    s_stop_requested = true;

    // Do not disable torque - let current motion finish naturally
    // The action task will check s_stop_requested and stop gracefully

    return ESP_OK;
}

esp_err_t motion_resume(void)
{
    if (!s_emergency_stop && !s_stop_requested) {
        ESP_LOGW(TAG, "Not in stopped state, nothing to resume");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Resuming from stop state");

    s_emergency_stop = false;
    s_stop_requested = false;

    // Re-enable torque on all servos
    servo_em3_enable_torque(MOTION_SERVO_EM3_ID, 1);

    // Re-enable LX servo torque by moving to current position
    for (int i = 0; i < MOTION_SERVO_LX_COUNT; i++) {
        int16_t pos = servo_lx_read_pos(s_lx_ids[i]);
        if (pos >= 0) {
            servo_lx_move(s_lx_ids[i], pos, 0);
        }
    }

    if (s_state == MOTION_STATE_EMERGENCY_STOP) {
        if (s_homing_completed) {
            set_state(MOTION_STATE_READY);
        } else {
            set_state(MOTION_STATE_IDLE);
        }
    }

    return ESP_OK;
}

esp_err_t motion_set_interrupt_callback(motion_interrupt_cb_t cb)
{
    s_interrupt_cb = cb;
    return ESP_OK;
}

bool motion_is_emergency_stop(void)
{
    return s_emergency_stop;
}

// ==================== Main Init/Deinit ====================

esp_err_t motion_controller_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Motion controller already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing motion controller...");

    // Create mutex
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    // Reset all state
    s_state = MOTION_STATE_IDLE;
    s_mode = MOTION_MODE_NORMAL;
    s_emergency_stop = false;
    s_stop_requested = false;
    s_homing_completed = false;
    s_action_status = ACTION_STATUS_IDLE;
    s_current_action = NULL;
    s_action_task_handle = NULL;
    s_homing_task_handle = NULL;
    s_interrupt_cb = NULL;
    s_action_cb = NULL;

    // Clear pose storage
    memset(s_saved_poses, 0, sizeof(s_saved_poses));

    s_initialized = true;

    ESP_LOGI(TAG, "Motion controller initialized successfully");
    ESP_LOGI(TAG, "  EM3 ID: %d, LX IDs: [%d, %d, %d, %d]",
             MOTION_SERVO_EM3_ID, s_lx_ids[0], s_lx_ids[1], s_lx_ids[2], s_lx_ids[3]);
    ESP_LOGI(TAG, "  Home positions: EM3=%d, LX=[%d,%d,%d,%d]",
             MOTION_HOME_EM3_POS, MOTION_HOME_LX1_POS, MOTION_HOME_LX2_POS,
             MOTION_HOME_LX3_POS, MOTION_HOME_LX5_POS);

    return ESP_OK;
}

esp_err_t motion_controller_deinit(void)
{
    if (!s_initialized) {
        ESP_LOGW(TAG, "Motion controller not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Deinitializing motion controller...");

    // Stop any running actions
    s_stop_requested = true;
    s_emergency_stop = true;

    // Wait for tasks to finish
    int timeout_ms = 2000;
    while ((s_action_task_handle != NULL || s_homing_task_handle != NULL) && timeout_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(50));
        timeout_ms -= 50;
    }

    if (s_action_task_handle != NULL) {
        ESP_LOGW(TAG, "Action task did not finish in time");
    }

    if (s_homing_task_handle != NULL) {
        ESP_LOGW(TAG, "Homing task did not finish in time");
    }

    // Disable torque on all servos
    servo_em3_enable_torque(MOTION_SERVO_EM3_ID, 0);
    servo_lx_unload_all(s_lx_ids, MOTION_SERVO_LX_COUNT);

    // Delete mutex
    if (s_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }

    // Reset state
    s_state = MOTION_STATE_UNINIT;
    s_initialized = false;
    s_homing_completed = false;
    s_emergency_stop = false;
    s_stop_requested = false;
    s_action_status = ACTION_STATUS_IDLE;
    s_current_action = NULL;
    s_interrupt_cb = NULL;
    s_action_cb = NULL;

    memset(s_saved_poses, 0, sizeof(s_saved_poses));

    ESP_LOGI(TAG, "Motion controller deinitialized");

    return ESP_OK;
}

motion_controller_status_t motion_controller_get_status(void)
{
    motion_controller_status_t status = {0};

    status.state = s_state;
    status.mode = s_mode;
    status.homing_completed = s_homing_completed;
    status.action_status = s_action_status;
    status.current_action = s_current_action;

    for (int i = 0; i < POSE_SLOT_MAX; i++) {
        status.pose_saved[i] = s_saved_poses[i].valid;
    }

    return status;
}

// ==================== Voice Command Interface (Reserved) ====================

esp_err_t motion_voice_command_execute(const char* command)
{
    if (!command) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_state == MOTION_STATE_UNINIT) {
        ESP_LOGW(TAG, "Motion controller not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Voice command received: %s", command);

    // Map voice commands to actions
    if (strcmp(command, "你过来") == 0 || strcmp(command, "come_here") == 0) {
        return motion_action_execute_async("come_here", NULL);
    } else if (strcmp(command, "回去吧") == 0 || strcmp(command, "go_back") == 0) {
        return motion_pose_restore(POSE_SLOT_INTERACTION_BEFORE);
    } else if (strcmp(command, "跳个舞吧") == 0 || strcmp(command, "dance") == 0) {
        return motion_action_execute_async("dance", NULL);
    } else if (strcmp(command, "打招呼") == 0 || strcmp(command, "greet") == 0) {
        return motion_action_execute_async("greet", NULL);
    } else if (strcmp(command, "回家") == 0 || strcmp(command, "home") == 0) {
        return motion_home_async_execute();
    } else if (strcmp(command, "休息") == 0 || strcmp(command, "rest") == 0) {
        return motion_action_execute_async("breath", NULL);
    } else if (strcmp(command, "专注") == 0 || strcmp(command, "focus") == 0) {
        return motion_action_execute_async("focus", NULL);
    } else {
        ESP_LOGW(TAG, "Unknown voice command: %s", command);
        return ESP_ERR_NOT_FOUND;
    }
}

// ==================== Scene Trigger Interface (Reserved) ====================

esp_err_t motion_scene_trigger(const char* scene_name)
{
    if (!scene_name) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_state == MOTION_STATE_UNINIT) {
        ESP_LOGW(TAG, "Motion controller not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Scene trigger: %s", scene_name);

    // Map scene names to action sequences
    if (strcmp(scene_name, "welcome") == 0) {
        // Welcome scene: home + greet
        motion_home_async_execute();
        return motion_action_execute_async("greet", NULL);
    } else if (strcmp(scene_name, "singing") == 0) {
        // Singing scene: dance with medium intensity
        motion_action_params_t params = s_default_params;
        params.intensity = 60;
        params.repeat_count = 5;
        return motion_action_execute_async("dance", &params);
    } else if (strcmp(scene_name, "exercise") == 0) {
        // Exercise scene
        motion_action_params_t params = s_default_params;
        params.intensity = 70;
        params.repeat_count = 3;
        return motion_action_execute_async("exercise", &params);
    } else if (strcmp(scene_name, "focus_time") == 0) {
        // Focus time scene: set focus mode + minimal movement
        motion_controller_set_mode(MOTION_MODE_FOCUS);
        return motion_action_execute_async("focus", NULL);
    } else {
        ESP_LOGW(TAG, "Unknown scene: %s", scene_name);
        return ESP_ERR_NOT_FOUND;
    }
}

#endif // MOTION_CONTROLLER_ENABLE
