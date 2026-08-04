/*
 * motion_analyzer.h - 运动分析接口
 *
 * 根据雷达位置数据检测人体运动状态。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "radar_module.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== Configuration ===================== */
#define MOTION_POSITION_HISTORY_SIZE        5
#define MOTION_VELOCITY_AVERAGE_WINDOW_SIZE 5
#define MOTION_POSITION_AVERAGE_WINDOW_SIZE 5

#define MOTION_POSITION_THRESHOLD_CM        10.0f
#define MOTION_VELOCITY_THRESHOLD           0.5f
#define MOTION_DURATION_THRESHOLD_MS        1000
#define MOTION_HISTORY_MAX_RECORDS          10
#define MOTION_TOLERANCE_COUNT              3

#define MOTION_POSITION_FILTER_ALPHA        0.3f
#define MOTION_VELOCITY_FILTER_ALPHA        0.3f
#define MOTION_HUMAN_PRESENT_TOLERANCE_COUNT 3
#define MOTION_VELOCITY_DEAD_ZONE_THRESHOLD 0.1f
#define MOTION_VELOCITY_JUMP_THRESHOLD      2.0f
#define MOTION_POSITION_STABLE_THRESHOLD_CM 5.0f
#define MOTION_STATE_CHECK_INTERVAL_MS      100

/* ===================== Data Structures ===================== */
typedef struct {
    int64_t start_time_ms;
    int64_t end_time_ms;
    int64_t duration_ms;
} motion_history_record_t;

typedef struct {
    float   x;
    float   y;
    float   z;
    int64_t timestamp_ms;
} motion_position_t;

typedef void (*motion_state_callback_t)(radar_motion_state_t old_state,
                                        radar_motion_state_t new_state,
                                        void *user_data);

/* ===================== Public API ===================== */

esp_err_t motion_analyzer_init(void);
esp_err_t motion_analyzer_deinit(void);

esp_err_t motion_analyzer_enable(void);
esp_err_t motion_analyzer_disable(void);
bool      motion_analyzer_is_enabled(void);

radar_motion_state_t motion_analyzer_get_state(void);
int64_t              motion_analyzer_get_sedentary_duration_ms(void);

esp_err_t motion_analyzer_update_position(float x, float y, float z,
                                          int32_t dop_idx, int64_t timestamp_ms);

esp_err_t motion_analyzer_update_human_presence(bool is_present);

esp_err_t motion_analyzer_register_callback(motion_state_callback_t callback, void *user_data);
esp_err_t motion_analyzer_unregister_callback(void);

bool motion_analyzer_is_initialized(void);

#ifdef __cplusplus
}
#endif