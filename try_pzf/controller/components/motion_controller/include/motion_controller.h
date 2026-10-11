#pragma once
#include "esp_err.h"
#include "motion_controller_types.h"

/* Async ESP_OK means accepted, NOT that movement has already completed.
 * Serial errors and final position feedback are reported by the worker log/status. */
esp_err_t motion_controller_init(void);
esp_err_t motion_controller_get_status(motion_controller_status_t *out);
esp_err_t motion_action_execute_async(const char *name, const motion_action_params_t *params);
esp_err_t motion_action_stop(void); /* Soft cancellation; waits for bus ownership release. */
motion_action_status_t motion_action_get_status(void);
esp_err_t motion_servo_set_position_async(int servo_index, int position);
esp_err_t motion_home_async(int time_ms);
esp_err_t motion_gesture_async(const char *gesture);
esp_err_t motion_voice_command(const char *command);
void motion_log_positions(void); /* Startup read-only diagnostic, never moves motors. */

/* Internal action-library interface; callers outside this component must not use these. */
bool motion_library_has_action(const char *name);
void motion_library_run(const char *name, const motion_action_params_t *params);
void motion_send_pose(const int16_t pos[5], uint16_t time_ms, uint16_t speed);
