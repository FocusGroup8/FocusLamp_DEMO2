/*
 * servo_service.h - Servo control service for FocusLamp
 *
 * Provides:
 *   - Basic servo position/speed control (via event bus)
 *   - Recording / Playback of servo motion sequences
 */

#pragma once
#ifndef __SERVO_SERVICE_H__
#define __SERVO_SERVICE_H__

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#include "servo_service_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#if (SERVO_SERVICE_ENABLE == 1)

/* ===================== Recording State ===================== */
typedef enum {
    SERVO_SERVICE_STATE_IDLE,
    SERVO_SERVICE_STATE_RECORDING,
    SERVO_SERVICE_STATE_HAS_DATA,
    SERVO_SERVICE_STATE_PLAYING
} servo_service_state_t;

/* ===================== Motion Frame ===================== */
typedef struct {
    int16_t  lx_pos[4];   /* LX servo positions (IDs 1,2,3,5) */
    int16_t  em3_pos;     /* EM3 servo position (ID 4) */
    uint32_t time_ms;     /* Timestamp relative to recording start */
} servo_service_frame_t;

/* ===================== Service Status ===================== */
typedef struct {
    servo_service_state_t current_state;
    int                   current_slot;
    int                   frame_count;
} servo_service_status_t;

/* ===================== Parameters ===================== */
typedef struct {
    uint16_t sampling_ms;
    uint16_t playback_speed;  /* 10 = 1.0x, 20 = 2.0x, 5 = 0.5x */
    uint16_t max_frames;
    uint8_t  max_slots;
} servo_service_params_t;

/* ===================== Unified 5-Servo Position ===================== */
#define SERVO_SERVICE_LX_COUNT 4

typedef struct {
    int16_t em3_pos;              /* EM3 servo position (0-3000) */
    int16_t lx_pos[SERVO_SERVICE_LX_COUNT]; /* LX servo positions (0-1000), IDs 1,2,3,5 */
} servo_service_positions_t;

/* ===================== Position Limits ===================== */
typedef struct {
    int16_t em3_min;
    int16_t em3_max;
    int16_t lx_min;
    int16_t lx_max;
} servo_service_limits_t;

#endif /* SERVO_SERVICE_ENABLE */

/**
 * @brief Initialize servo service.
 *        Subscribes to EV_SERVO_* events and initializes servo_driver.
 * @return esp_err_t
 */
esp_err_t servo_service_init(void);

/**
 * @brief Deinitialize servo service.
 * @return esp_err_t
 */
esp_err_t servo_service_deinit(void);

/**
 * @brief Set target position of a servo.
 * @param servo_id  Servo ID (0-based)
 * @param position  Target position (protocol-specific range)
 * @return esp_err_t
 */
esp_err_t servo_service_set_position(uint8_t servo_id, uint16_t position);

/**
 * @brief Set movement speed of a servo.
 * @param servo_id  Servo ID (0-based)
 * @param speed     Speed value (protocol-specific range)
 * @return esp_err_t
 */
esp_err_t servo_service_set_speed(uint8_t servo_id, uint16_t speed);

/**
 * @brief Enable all servos (set OE pins high).
 * @return esp_err_t
 */
esp_err_t servo_service_enable(void);

/**
 * @brief Disable all servos (set OE pins low).
 * @return esp_err_t
 */
esp_err_t servo_service_disable(void);

/**
 * @brief Get current position of a servo.
 * @param servo_id  Servo ID (0-based)
 * @return uint16_t Current position, 0xFFFF if unknown
 */
uint16_t servo_service_get_position(uint8_t servo_id);

/**
 * @brief Set positions of all 5 servos (1 EM3 + 4 LX) in one call.
 * @param positions  Target positions for all servos
 * @param time_ms    Movement duration in milliseconds (used for LX; EM3 uses calculated speed)
 * @return esp_err_t
 */
esp_err_t servo_service_set_all_positions(const servo_service_positions_t *positions, uint16_t time_ms);

/**
 * @brief Move all servos to home position.
 * @param time_ms  Movement duration in milliseconds
 * @return esp_err_t
 */
esp_err_t servo_service_go_home(uint16_t time_ms);

/**
 * @brief Get default home positions.
 * @param[out] positions  Home positions to fill
 * @return esp_err_t
 */
esp_err_t servo_service_get_home_positions(servo_service_positions_t *positions);

/**
 * @brief Clamp a position to valid limits.
 * @param[in,out] positions  Positions to clamp
 * @return esp_err_t
 */
esp_err_t servo_service_clamp_positions(servo_service_positions_t *positions);

/**
 * @brief Smoothly move all servos from current to target positions.
 *        Blocks until movement completes or timeout.
 * @param target   Target positions
 * @param time_ms  Total movement duration in milliseconds
 * @return esp_err_t
 */
esp_err_t servo_service_smooth_move(const servo_service_positions_t *target, uint16_t time_ms);

/**
 * @brief Set position limits.
 * @param limits  Limit values
 * @return esp_err_t
 */
esp_err_t servo_service_set_limits(const servo_service_limits_t *limits);

/**
 * @brief Get current position limits.
 * @param[out] limits  Limit values to fill
 * @return esp_err_t
 */
esp_err_t servo_service_get_limits(servo_service_limits_t *limits);

#if (SERVO_SERVICE_ENABLE == 1)

/* ===================== Recording / Playback API ===================== */

/**
 * @brief Get current service status.
 * @return servo_service_status_t
 */
servo_service_status_t servo_service_get_status(void);

/**
 * @brief Get frame count for a specific slot.
 * @param slot  Slot index
 * @return int  Frame count
 */
int servo_service_get_frame_count(int slot);

/**
 * @brief Start recording servo positions.
 */
void servo_service_start_recording(void);

/**
 * @brief Stop recording and save to storage.
 */
void servo_service_stop_recording(void);

/**
 * @brief Start playback of recorded motion.
 */
void servo_service_start_playback(void);

/**
 * @brief Stop playback.
 */
void servo_service_stop_playback(void);

/**
 * @brief Switch to a different recording slot.
 * @param slot  Slot index
 */
void servo_service_switch_slot(int slot);

/**
 * @brief Set recording/playback parameters.
 * @param params  Parameters struct
 * @return esp_err_t
 */
esp_err_t servo_service_set_params(const servo_service_params_t *params);

/**
 * @brief Get current recording/playback parameters.
 * @param params  Output parameters struct
 * @return esp_err_t
 */
esp_err_t servo_service_get_params(servo_service_params_t *params);

#endif /* SERVO_SERVICE_ENABLE */

#ifdef __cplusplus
}
#endif

#endif /* __SERVO_SERVICE_H__ */
