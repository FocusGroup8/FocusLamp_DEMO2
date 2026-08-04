#ifndef MOTION_CONTROLLER_H
#define MOTION_CONTROLLER_H

#include "esp_err.h"

#include "motion_controller_types.h"
#include "motion_controller_config.h"

#ifdef __cplusplus
extern "C"
{
#endif

#if (MOTION_CONTROLLER_ENABLE == 1)

/**
 * @brief Initialize motion controller module
 *
 * @return esp_err_t
 */
esp_err_t motion_controller_init(void);

/**
 * @brief Deinitialize motion controller module
 *
 * @return esp_err_t
 */
esp_err_t motion_controller_deinit(void);

/**
 * @brief Get current motion controller status
 *
 * @return motion_controller_status_t
 */
motion_controller_status_t motion_controller_get_status(void);

/**
 * @brief Set motion mode (for safety limiting)
 *
 * @param mode Motion mode
 * @return esp_err_t
 */
esp_err_t motion_controller_set_mode(motion_mode_t mode);

/**
 * @brief Get current motion mode
 *
 * @return motion_mode_t
 */
motion_mode_t motion_controller_get_mode(void);

// ==================== Home Manager Interface ====================

/**
 * @brief Execute homing sequence (blocking)
 *
 * @return esp_err_t
 */
esp_err_t motion_home_execute(void);

/**
 * @brief Execute homing sequence (async, background task)
 *
 * @return esp_err_t
 */
esp_err_t motion_home_async_execute(void);

/**
 * @brief Check if homing is completed
 *
 * @return bool
 */
bool motion_home_is_completed(void);

/**
 * @brief Execute startup test sequence (move servos slightly then return to home)
 *
 * @return esp_err_t
 */
esp_err_t motion_startup_test(void);

// ==================== Pose Manager Interface ====================

/**
 * @brief Save current pose to specified slot
 *
 * @param slot Pose slot
 * @return esp_err_t
 */
esp_err_t motion_pose_save(motion_pose_slot_t slot);

/**
 * @brief Restore pose from specified slot
 *
 * @param slot Pose slot
 * @return esp_err_t
 */
esp_err_t motion_pose_restore(motion_pose_slot_t slot);

/**
 * @brief Get pose from specified slot
 *
 * @param slot Pose slot
 * @param pose Output pose structure
 * @return esp_err_t
 */
esp_err_t motion_pose_get(motion_pose_slot_t slot, motion_pose_t* pose);

/**
 * @brief Check if pose slot has valid data
 *
 * @param slot Pose slot
 * @return bool
 */
bool motion_pose_is_valid(motion_pose_slot_t slot);

// ==================== Safety Limiter Interface ====================

/**
 * @brief Check if position is within safety limits
 *
 * @param servo_id Servo ID
 * @param position Position value
 * @return bool
 */
bool motion_safety_check_position(uint8_t servo_id, int16_t position);

/**
 * @brief Clamp position to safety limits
 *
 * @param servo_id Servo ID
 * @param position Input position
 * @return int16_t Clamped position
 */
int16_t motion_safety_clamp_position(uint8_t servo_id, int16_t position);

/**
 * @brief Clamp speed to safety limits based on current mode
 *
 * @param servo_id Servo ID
 * @param speed Input speed
 * @return uint16_t Clamped speed
 */
uint16_t motion_safety_clamp_speed(uint8_t servo_id, uint16_t speed);

/**
 * @brief Get safety limits for current mode
 *
 * @param limits Output limits structure
 * @return esp_err_t
 */
esp_err_t motion_safety_get_limits(motion_safety_limits_t* limits);

// ==================== Interrupt Handler Interface ====================

/**
 * @brief Execute emergency stop (immediate halt)
 *
 * @return esp_err_t
 */
esp_err_t motion_emergency_stop(void);

/**
 * @brief Execute soft stop (smooth stop)
 *
 * @return esp_err_t
 */
esp_err_t motion_soft_stop(void);

/**
 * @brief Resume after stop
 *
 * @return esp_err_t
 */
esp_err_t motion_resume(void);

/**
 * @brief Set interrupt callback
 *
 * @param cb Callback function
 * @return esp_err_t
 */
esp_err_t motion_set_interrupt_callback(motion_interrupt_cb_t cb);

/**
 * @brief Check if in emergency stop state
 *
 * @return bool
 */
bool motion_is_emergency_stop(void);

// ==================== Action Library Interface ====================

/**
 * @brief Execute preset action (blocking)
 *
 * @param action_name Action name
 * @param params Action parameters (NULL for default)
 * @return esp_err_t
 */
esp_err_t motion_action_execute(const char* action_name, const motion_action_params_t* params);

/**
 * @brief Execute preset action (async)
 *
 * @param action_name Action name
 * @param params Action parameters (NULL for default)
 * @return esp_err_t
 */
esp_err_t motion_action_execute_async(const char* action_name, const motion_action_params_t* params);

/**
 * @brief Stop current action
 *
 * @return esp_err_t
 */
esp_err_t motion_action_stop(void);

/**
 * @brief Get current action status
 *
 * @return motion_action_status_t
 */
motion_action_status_t motion_action_get_status(void);

/**
 * @brief Set action complete callback
 *
 * @param cb Callback function
 * @return esp_err_t
 */
esp_err_t motion_set_action_callback(motion_action_complete_cb_t cb);

// ==================== Voice Command Interface (Reserved) ====================

/**
 * @brief Execute voice command (reserved interface for voice module)
 *
 * @param command Voice command string
 * @return esp_err_t
 */
esp_err_t motion_voice_command_execute(const char* command);

// ==================== Scene Trigger Interface (Reserved) ====================

/**
 * @brief Trigger scene action (reserved interface for scene manager)
 *
 * @param scene_name Scene name
 * @return esp_err_t
 */
esp_err_t motion_scene_trigger(const char* scene_name);

#else

// Stub implementations when module is disabled

static inline esp_err_t motion_controller_init(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t motion_controller_deinit(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline motion_controller_status_t motion_controller_get_status(void) { return (motion_controller_status_t){0}; }
static inline esp_err_t motion_controller_set_mode(motion_mode_t mode) { (void)mode; return ESP_ERR_NOT_SUPPORTED; }
static inline motion_mode_t motion_controller_get_mode(void) { return MOTION_MODE_NORMAL; }
static inline esp_err_t motion_home_execute(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t motion_home_async_execute(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline bool motion_home_is_completed(void) { return false; }
static inline esp_err_t motion_pose_save(motion_pose_slot_t slot) { (void)slot; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t motion_pose_restore(motion_pose_slot_t slot) { (void)slot; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t motion_pose_get(motion_pose_slot_t slot, motion_pose_t* pose) { (void)slot; (void)pose; return ESP_ERR_NOT_SUPPORTED; }
static inline bool motion_pose_is_valid(motion_pose_slot_t slot) { (void)slot; return false; }
static inline bool motion_safety_check_position(uint8_t servo_id, int16_t position) { (void)servo_id; (void)position; return true; }
static inline int16_t motion_safety_clamp_position(uint8_t servo_id, int16_t position) { (void)servo_id; return position; }
static inline uint16_t motion_safety_clamp_speed(uint8_t servo_id, uint16_t speed) { (void)servo_id; return speed; }
static inline esp_err_t motion_safety_get_limits(motion_safety_limits_t* limits) { (void)limits; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t motion_emergency_stop(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t motion_soft_stop(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t motion_resume(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t motion_set_interrupt_callback(motion_interrupt_cb_t cb) { (void)cb; return ESP_ERR_NOT_SUPPORTED; }
static inline bool motion_is_emergency_stop(void) { return false; }
static inline esp_err_t motion_action_execute(const char* action_name, const motion_action_params_t* params) { (void)action_name; (void)params; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t motion_action_execute_async(const char* action_name, const motion_action_params_t* params) { (void)action_name; (void)params; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t motion_action_stop(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline motion_action_status_t motion_action_get_status(void) { return ACTION_STATUS_IDLE; }
static inline esp_err_t motion_set_action_callback(motion_action_complete_cb_t cb) { (void)cb; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t motion_voice_command_execute(const char* command) { (void)command; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t motion_scene_trigger(const char* scene_name) { (void)scene_name; return ESP_ERR_NOT_SUPPORTED; }

#endif

#ifdef __cplusplus
}
#endif

#endif // MOTION_CONTROLLER_H