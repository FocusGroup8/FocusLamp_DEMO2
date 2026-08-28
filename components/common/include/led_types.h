/*
 * led_types.h - Shared LED type definitions
 */

#pragma once
#ifndef __LED_TYPES_H__
#define __LED_TYPES_H__

#include <stdint.h>
#include <stdbool.h>

#include "data_type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== WS2812 Color Type ===================== */
typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} ws2812_rgb_t;

/* ===================== LED Blink Mode ===================== */
typedef enum {
    LED_BLINK_MODE_NONE = 0,
    LED_BLINK_MODE_FAST,
    LED_BLINK_MODE_SLOW,
} led_blink_mode_t;

/* ===================== LED Module Mode ===================== */
typedef enum {
    LED_MODULE_MODE_OFF = 0,
    LED_MODULE_MODE_SOLID,
    LED_MODULE_MODE_BREATH,
    LED_MODULE_MODE_BLINK,
    LED_MODULE_MODE_PRESENCE,
} led_module_mode_t;

/* ===================== LED Module System State ===================== */
typedef enum {
    LED_MODULE_SYSTEM_STATE_IDLE = 0,
    LED_MODULE_SYSTEM_STATE_STARTING,
    LED_MODULE_SYSTEM_STATE_RUNNING,
    LED_MODULE_SYSTEM_STATE_WARNING,
    LED_MODULE_SYSTEM_STATE_ERROR,
} led_module_system_state_t;

/* ===================== Radar Motion State ===================== */
typedef enum {
    RADAR_MOTION_STATE_IDLE = 0,
    RADAR_MOTION_STATE_ACTIVE,
    RADAR_MOTION_STATE_MICRO_MOTION,
    RADAR_MOTION_STATE_STATIONARY,
    RADAR_MOTION_STATE_SEDENTARY,
} radar_motion_state_t;

#define RADAR_MOTION_IDLE         RADAR_MOTION_STATE_IDLE
#define RADAR_MOTION_ACTIVE       RADAR_MOTION_STATE_ACTIVE
#define RADAR_MOTION_MICRO_MOTION RADAR_MOTION_STATE_MICRO_MOTION
#define RADAR_MOTION_STATIONARY   RADAR_MOTION_STATE_STATIONARY
#define RADAR_MOTION_SEDENTARY    RADAR_MOTION_STATE_SEDENTARY

/* ===================== LED Snapshot ===================== */
typedef struct {
    led_module_mode_t mode;
    ws2812_rgb_t      color;
    uint8_t           brightness;
    uint8_t           brightness_level; /* 0-5 */
    bool              initialized;
} led_module_snapshot_t;

/* ===================== Breathing Effect Defaults ===================== */
#define LED_BREATH_DEFAULT_PERIOD_MS    3000
#define LED_BREATH_MIN_BRIGHTNESS       5
#define LED_BREATH_MAX_BRIGHTNESS       30
#define LED_AUTO_OFF_TIMEOUT_MS         60000

/* Default breath color (pink) */
#define LED_COLOR_BREATH_RED            255
#define LED_COLOR_BREATH_GREEN          105
#define LED_COLOR_BREATH_BLUE           180

#ifdef __cplusplus
}
#endif

#endif /* __LED_TYPES_H__ */
