#ifndef MOTION_CONTROLLER_TYPES_H
#define MOTION_CONTROLLER_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief Motion controller state
 */
typedef enum
{
    MOTION_STATE_UNINIT = 0,
    MOTION_STATE_IDLE,
    MOTION_STATE_HOMING,
    MOTION_STATE_READY,
    MOTION_STATE_EXECUTING,
    MOTION_STATE_EMERGENCY_STOP,
    MOTION_STATE_ERROR
} motion_state_t;

/**
 * @brief Motion mode (for safety limiting)
 */
typedef enum
{
    MOTION_MODE_NORMAL = 0,    // Normal mode: standard limits
    MOTION_MODE_FOCUS = 1,     // Focus mode: small amplitude, slow speed
    MOTION_MODE_COMPANY = 2,   // Company mode: light movements
    MOTION_MODE_SHOW = 3,      // Show mode: allow large movements
} motion_mode_t;

/**
 * @brief Pose slot for pose manager
 */
typedef enum
{
    POSE_SLOT_INTERACTION_BEFORE = 0,  // Pose before interaction ("go back" uses this)
    POSE_SLOT_SAFE_STANDBY = 1,        // Safe standby pose
    POSE_SLOT_HOME = 2,                // Home position
    POSE_SLOT_CUSTOM_1 = 3,            // Custom pose 1
    POSE_SLOT_CUSTOM_2 = 4,            // Custom pose 2
    POSE_SLOT_MAX = 5
} motion_pose_slot_t;

/**
 * @brief Servo pose structure
 */
typedef struct
{
    int16_t em3_pos;      // EM3 servo position (ID4)
    int16_t lx_pos[4];    // LX servo positions (ID1, ID2, ID3, ID5)
    uint32_t timestamp;   // Timestamp when pose was saved
    bool valid;           // Whether pose data is valid
} motion_pose_t;

/**
 * @brief Action parameters
 */
typedef struct
{
    int intensity;        // Action intensity (0-100)
    int speed;            // Action speed (0-100)
    int repeat_count;     // Repeat count (default 1)
    motion_mode_t mode;   // Execution mode
} motion_action_params_t;

/**
 * @brief Action execution status
 */
typedef enum
{
    ACTION_STATUS_IDLE = 0,
    ACTION_STATUS_RUNNING,
    ACTION_STATUS_COMPLETED,
    ACTION_STATUS_INTERRUPTED,
    ACTION_STATUS_FAILED
} motion_action_status_t;

/**
 * @brief Safety limit configuration
 */
typedef struct
{
    int16_t em3_min_pos;
    int16_t em3_max_pos;
    int16_t lx_min_pos;
    int16_t lx_max_pos;
    uint16_t em3_max_speed;
    uint16_t lx_max_speed;
    uint16_t em3_max_accel;
    uint16_t lx_max_accel;
} motion_safety_limits_t;

/**
 * @brief Motion controller status
 */
typedef struct
{
    motion_state_t state;
    motion_mode_t mode;
    bool homing_completed;
    bool pose_saved[POSE_SLOT_MAX];
    motion_action_status_t action_status;
    const char* current_action;
} motion_controller_status_t;

/**
 * @brief Interrupt callback type
 */
typedef void (*motion_interrupt_cb_t)(motion_state_t old_state, motion_state_t new_state);

/**
 * @brief Action complete callback type
 */
typedef void (*motion_action_complete_cb_t)(const char* action_name, motion_action_status_t status);

#ifdef __cplusplus
}
#endif

#endif // MOTION_CONTROLLER_TYPES_H