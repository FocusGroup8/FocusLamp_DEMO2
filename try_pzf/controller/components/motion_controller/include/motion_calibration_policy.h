#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "motion_manual_limits.h"

/* An explicitly chosen trial interval is not a mechanically certified limit.
 * This RAM-only configuration never modifies the formal manual limits. */
typedef struct {
    int servo_id;
    int min_position, max_position;
    int step;
    int time_ms, speed;
} motion_calibration_config_t;

static inline int motion_calibration_axis(int servo_id)
{
    return servo_id==4 ? 0 : servo_id>=1 && servo_id<=3 ? servo_id : servo_id==5 ? 4 : -1;
}

static inline bool motion_calibration_config_valid(const motion_calibration_config_t *config,
                                                   int protocol_min,int protocol_max)
{
    if (!config || motion_calibration_axis(config->servo_id)<0 || protocol_min>protocol_max ||
        config->min_position<0 || config->max_position>(config->servo_id==4 ? 3000 : 1000) ||
        config->min_position<protocol_min || config->max_position>protocol_max ||
        config->min_position>=config->max_position || config->step<=0 ||
        (int64_t)config->step>(int64_t)config->max_position-config->min_position) return false;
    if (config->servo_id==4) return config->time_ms==0 &&
        config->speed>=MOTION_MANUAL_SPEED_MIN && config->speed<=MOTION_MANUAL_SPEED_MAX;
    return config->speed==0 && config->time_ms>=MOTION_MANUAL_TIME_MS_MIN &&
        config->time_ms<=MOTION_MANUAL_TIME_MS_MAX;
}

typedef enum {
    MOTION_CALIBRATION_TARGET_OK=0,
    MOTION_CALIBRATION_CURRENT_OUTSIDE,
    MOTION_CALIBRATION_TRIAL_BOUNDARY,
    MOTION_CALIBRATION_BAD_STEP
} motion_calibration_target_result_t;

/* No clamp: a boundary request is refused without producing a target. */
static inline motion_calibration_target_result_t motion_calibration_target(
    const motion_calibration_config_t *config,int current,int direction,int *target)
{
    if (!config || !target || (direction!=-1 && direction!=1) || config->step<=0)
        return MOTION_CALIBRATION_BAD_STEP;
    if (current<config->min_position || current>config->max_position)
        return MOTION_CALIBRATION_CURRENT_OUTSIDE;
    int64_t next=(int64_t)current+(int64_t)direction*config->step;
    if (next<config->min_position || next>config->max_position)
        return MOTION_CALIBRATION_TRIAL_BOUNDARY;
    *target=(int)next;
    return MOTION_CALIBRATION_TARGET_OK;
}
