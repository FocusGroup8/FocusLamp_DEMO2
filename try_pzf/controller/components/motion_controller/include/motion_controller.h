#pragma once
#include "esp_err.h"
#include "motion_controller_types.h"
#include "motion_position_cache.h"
#include "motion_manual_limits.h"
#include "motion_calibration_policy.h"

/* Async ESP_OK means accepted, NOT that movement has already completed.
 * Serial errors and final position feedback are reported by the worker log/status. */
esp_err_t motion_controller_init(void);
esp_err_t motion_controller_get_status(motion_controller_status_t *out);
esp_err_t motion_action_execute_async(const char *name, const motion_action_params_t *params);
esp_err_t motion_action_stop(void); /* Soft cancellation; waits for bus ownership release. */
motion_action_status_t motion_action_get_status(void);
esp_err_t motion_servo_set_position_async(int servo_index, int position);

typedef struct {
    int servo_id; /* Bottom-to-top 1..5; also the physical bus ID. */
    int axis;     /* Existing internal axis order: 1,2,3,0,4. */
    bool configured;
    int min_position;
    int max_position;
} motion_manual_servo_limits_t;

/* Reads compile-time manual limits only; never reads or changes a servo. */
esp_err_t motion_manual_servo_get_limits(int servo_id, motion_manual_servo_limits_t *out);
/* Manual-only worker path, rejects busy rather than queueing. LX uses time_ms
 * (0 selects default) and requires speed=0. EM3 uses speed (0 selects default)
 * and requires time_ms=0. ESP_OK means accepted, never physical completion.
 * NOT_SUPPORTED = limits unconfigured; INVALID_STATE = unready/busy;
 * INVALID_ARG = bad ID/parameter/target. Live preflight failures are logged by
 * the worker and reported as ACTION_STATUS_FAILED, before torque or movement. */
esp_err_t motion_manual_servo_async(int servo_id, int position, int time_ms, int speed);

typedef struct {
    bool active, ready, motion_busy;
    uint32_t session_id;
    motion_calibration_config_t config;
    bool reference_valid;
    int reference_raw;
    bool has_observations;
    int observed_min, observed_max;
    bool has_target;
    int target_raw;
    motion_axis_position_snapshot_t feedback;
    motion_action_status_t action_status;
    esp_err_t last_error;
    const char *reason; /* Fixed diagnostic literal, never caller-owned storage. */
} motion_calibration_snapshot_t;

/* RAM-only, one-axis trial mode; begin only reads a fresh reference. Both begin
 * and jog enqueue one existing executor job; ESP_OK never means arrival.
 * All ordinary movement entries are refused while the session is active.
 * A failed reference/feedback/write blocks later jogs until end + new begin.
 * A boundary refusal alone leaves the opposite direction available. */
esp_err_t motion_calibration_begin_async(const motion_calibration_config_t *config,
                                         uint32_t *session_id);
esp_err_t motion_calibration_jog_async(uint32_t session_id, int direction);
esp_err_t motion_calibration_end(uint32_t session_id);
/* Copies cache only; never performs a UART query. */
esp_err_t motion_calibration_get_snapshot(motion_calibration_snapshot_t *out);

esp_err_t motion_home_async(int time_ms);
esp_err_t motion_gesture_async(const char *gesture);
esp_err_t motion_voice_command(const char *command);
void motion_log_positions(void); /* Startup read-only diagnostic, never moves motors. */

/* Stage-one diagnostics: idle-only readback, no torque/movement/home commands.
 * The getter copies cache only and never performs a UART transaction. */
esp_err_t motion_controller_start_position_sampling(void);
esp_err_t motion_controller_get_position_snapshot(motion_position_snapshot_t *out);

/* Internal action-library interface; callers outside this component must not use these. */
bool motion_library_has_action(const char *name);
void motion_library_run(const char *name, const motion_action_params_t *params);
void motion_send_pose(const int16_t pos[5], uint16_t time_ms, uint16_t speed);
