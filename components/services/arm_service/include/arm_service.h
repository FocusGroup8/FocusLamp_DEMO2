/*
 * arm_service.h - Mechanical arm service for FocusLamp
 */

#pragma once
#ifndef __ARM_SERVICE_H__
#define __ARM_SERVICE_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== Action Step ===================== */
typedef struct {
    uint8_t  servo_id;      /* Target servo ID */
    uint16_t position;      /* Target position */
    uint32_t duration_ms;   /* Movement duration in ms */
    uint32_t delay_ms;      /* Delay after step in ms */
} action_step_t;

/* ===================== Action Sequence ===================== */
typedef struct {
    const action_step_t *steps;     /* Array of steps */
    uint16_t             step_count; /* Number of steps */
    uint8_t              loop_count; /* 0 = infinite, >0 = specific count */
} action_sequence_t;

/* ===================== Arm Status ===================== */
typedef enum {
    ARM_STATUS_IDLE     = 0,
    ARM_STATUS_RUNNING,
    ARM_STATUS_PAUSED,
    ARM_STATUS_ERROR,
} arm_status_t;

/**
 * @brief Initialize mechanical arm service.
 *        Subscribes to EV_ARM_* events and initializes servo_service.
 * @return esp_err_t
 */
esp_err_t arm_service_init(void);

/**
 * @brief Load an action sequence for execution.
 * @param seq  Pointer to action sequence definition
 * @return esp_err_t
 */
esp_err_t arm_service_load_action(const action_sequence_t *seq);

/**
 * @brief Start executing the loaded action sequence.
 * @return esp_err_t
 */
esp_err_t arm_service_start_action(void);

/**
 * @brief Emergency stop of current action sequence.
 * @return esp_err_t
 */
esp_err_t arm_service_stop_action(void);

/**
 * @brief Set speed multiplier for actions.
 * @param multiplier  Speed multiplier (0.1 = slow, 1.0 = normal, 2.0 = fast)
 * @return esp_err_t
 */
esp_err_t arm_service_set_speed_multiplier(float multiplier);

/**
 * @brief Get current arm status.
 * @return arm_status_t
 */
arm_status_t arm_service_get_status(void);

#ifdef __cplusplus
}
#endif

#endif /* __ARM_SERVICE_H__ */