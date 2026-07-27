/*
 * data_type.h - Common data type definitions for FocusLamp
 */

#pragma once
#ifndef __DATA_TYPE_H__
#define __DATA_TYPE_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== Color Types ===================== */
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_color_t;

typedef struct {
    uint16_t h;  /* 0-360 */
    uint8_t  s;  /* 0-100 */
    uint8_t  v;  /* 0-100 */
} hsv_color_t;

/* ===================== Geometry ===================== */
typedef struct {
    int16_t x;
    int16_t y;
} point_2d_t;

/* ===================== Touch Event ===================== */
typedef enum {
    TOUCH_EVENT_NONE        = 0,
    TOUCH_EVENT_SINGLE_CLICK,
    TOUCH_EVENT_DOUBLE_CLICK,
    TOUCH_EVENT_LONG_PRESS,
    TOUCH_EVENT_SLIDE_UP,
    TOUCH_EVENT_SLIDE_DOWN,
    TOUCH_EVENT_SLIDE_LEFT,
    TOUCH_EVENT_SLIDE_RIGHT,
    TOUCH_EVENT_RELEASE,
} touch_event_t;

/* ===================== System State ===================== */
typedef enum {
    SYSTEM_STATE_INIT          = 0,
    SYSTEM_STATE_STARTING,
    SYSTEM_STATE_RUNNING,
    SYSTEM_STATE_SLEEPING,
    SYSTEM_STATE_ERROR,
    SYSTEM_STATE_SHUTDOWN,
} system_state_t;

/* ===================== Application Mode ===================== */
typedef enum {
    APP_MODE_NORMAL_LIGHTING    = 0,  /* 普通照明 */
    APP_MODE_FOCUS,                   /* 专注 */
    APP_MODE_COMPANION,               /* 陪伴 */
    APP_MODE_ARM_ACTION,              /* 机械臂动作 */
    APP_MODE_MUSIC_RHYTHM,            /* 音乐律动 */
    APP_MODE_MINI_GAME,               /* 小游戏 */
    APP_MODE_VOICE_CONTROL,           /* 语音控制 */
    APP_MODE_MAX,
} app_mode_t;

#ifdef __cplusplus
}
#endif

#endif /* __DATA_TYPE_H__ */