#pragma once

/* Manual HTTP controls only. Existing actions, gestures and home retain their
 * existing behavior. Servo numbering follows the arm from BOTTOM to TOP:
 *   1=LX bus 1/axis 1, 2=LX bus 2/axis 2, 3=LX bus 3/axis 3,
 *   4=EM3 bus 4/axis 0, 5=LX bus 5/axis 4.
 *
 * User-confirmed usable boundaries, with clearance already left, 2026-10-25.
 * These are manually observed raw endpoints, not protocol full travel or
 * automatic calibration results. Keep fresh-current and target checks.
 * A current readback or the protocol's numeric range is NOT a safe travel range.
 * For each independently calibrated servo, enter integer MIN_RAW/MAX_RAW below
 * and set ENABLED to 1, then rebuild. -1 means unknown, never a usable limit.
 * Enabled but invalid/out-of-protocol-range entries also remain unconfigured.
 * The target AND a fresh current readback must be inside the configured interval
 * before the worker can enable torque or transmit a manual move.
 */
#define MOTION_MANUAL_SERVO_1_ENABLED 1
#define MOTION_MANUAL_SERVO_1_MIN_RAW 255
#define MOTION_MANUAL_SERVO_1_MAX_RAW 743

#define MOTION_MANUAL_SERVO_2_ENABLED 1
#define MOTION_MANUAL_SERVO_2_MIN_RAW 234
#define MOTION_MANUAL_SERVO_2_MAX_RAW 770

#define MOTION_MANUAL_SERVO_3_ENABLED 1
#define MOTION_MANUAL_SERVO_3_MIN_RAW 209
#define MOTION_MANUAL_SERVO_3_MAX_RAW 877

#define MOTION_MANUAL_SERVO_4_ENABLED 1
#define MOTION_MANUAL_SERVO_4_MIN_RAW 527
#define MOTION_MANUAL_SERVO_4_MAX_RAW 2350

#define MOTION_MANUAL_SERVO_5_ENABLED 1
#define MOTION_MANUAL_SERVO_5_MIN_RAW 100
#define MOTION_MANUAL_SERVO_5_MAX_RAW 924

#define MOTION_MANUAL_SERVO_COUNT 5
#define MOTION_MANUAL_TIME_MS_MIN 500
#define MOTION_MANUAL_TIME_MS_MAX 10000
#define MOTION_MANUAL_TIME_MS_DEFAULT 1500
#define MOTION_MANUAL_SPEED_MIN 1
#define MOTION_MANUAL_SPEED_MAX 500
#define MOTION_MANUAL_SPEED_DEFAULT 100
