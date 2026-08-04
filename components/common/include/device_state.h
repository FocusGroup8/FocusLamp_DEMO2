/*
 * device_state.h - Unified device state for FocusLamp
 *
 * Centralizes runtime state from all subsystems so that UI, console,
 * and application logic can query a single, consistent snapshot.
 */

#pragma once
#ifndef __DEVICE_STATE_H__
#define __DEVICE_STATE_H__

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== Application State ===================== */

typedef enum {
    APP_STATE_INIT          = 0,    /* 系统初始化中 */
    APP_STATE_IDLE,                 /* 空闲待机 */
    APP_STATE_LIGHTING,             /* 普通照明模式 */
    APP_STATE_FOCUS,                /* 专注模式 */
    APP_STATE_COMPANION,            /* 陪伴模式 */
    APP_STATE_ARM_ACTION,           /* 机械臂动作模式 */
    APP_STATE_MUSIC_RHYTHM,         /* 音乐律动模式 */
    APP_STATE_GAME,                 /* 小游戏模式 */
    APP_STATE_VOICE,                /* 语音控制模式 */
    APP_STATE_EXERCISE_FOLLOW,      /* 运动跟随模式 */
    APP_STATE_CUSTOM_RULE,          /* 自定义模式（规则引擎） */
    APP_STATE_SLEEP,                /* 休眠状态 */
    APP_STATE_ERROR,                /* 错误状态 */
} app_state_t;

/* ===================== Subsystem State ===================== */

typedef struct {
    uint8_t  level;          /* 0-5 */
    uint8_t  brightness;     /* 0-255 */
    uint8_t  r;
    uint8_t  g;
    uint8_t  b;
    bool     on;
} device_light_state_t;

typedef struct {
    uint8_t  level;          /* 0-5 */
    bool     muted;
    bool     playing;
} device_audio_state_t;

typedef struct {
    bool     present;
    float    heart_rate_bpm;
    float    breath_rate_bpm;
    float    distance_cm;
    float    hrv_sdnn;
    float    hrv_rmssd;
    bool     heart_rate_valid;
    bool     distance_valid;
} device_radar_state_t;

typedef struct {
    int16_t  em3_pos;
    int16_t  lx_pos[4];      /* LX IDs 1, 2, 3, 5 */
    bool     valid;
} device_servo_state_t;

typedef struct {
    uint8_t  level;          /* 0-4 */
    float    lux;
} device_ambient_light_state_t;

typedef struct {
    uint8_t  last_point;     /* Last active touch point */
    uint8_t  last_event;     /* touch_event_t */
    uint32_t last_event_time_ms;
    uint8_t  active_points;  /* Bitmask of currently active points */
} device_key_state_t;

/* ===================== Unified State ===================== */

typedef struct {
    /* System / App */
    app_state_t  app_state;
    uint32_t uptime_ms;

    /* Subsystems */
    device_light_state_t        light;
    device_audio_state_t        audio;
    device_radar_state_t        radar;
    device_servo_state_t        servo;
    device_ambient_light_state_t ambient_light;
    device_key_state_t          key;
} device_state_t;

/* ===================== Public API ===================== */

/**
 * @brief Initialize the unified device state.
 * @return esp_err_t
 */
esp_err_t device_state_init(void);

/**
 * @brief Get a copy of the current unified device state.
 * @param[out] state  Pointer to fill
 * @return esp_err_t
 */
esp_err_t device_state_get(device_state_t *state);

/**
 * @brief Update application state.
 */
esp_err_t device_state_set_app_state(app_state_t state);

/**
 * @brief Update light state.
 */
esp_err_t device_state_set_light(uint8_t level, uint8_t brightness, uint8_t r, uint8_t g, uint8_t b, bool on);

/**
 * @brief Update audio state.
 */
esp_err_t device_state_set_audio(uint8_t volume_level, bool muted, bool playing);

/**
 * @brief Update radar state.
 */
esp_err_t device_state_set_radar(bool present, float heart_rate, float breath_rate,
                                  float distance_cm, float hrv_sdnn, float hrv_rmssd);

/**
 * @brief Update servo state.
 */
esp_err_t device_state_set_servo(int16_t em3_pos, const int16_t lx_pos[4], bool valid);

/**
 * @brief Update ambient light state.
 */
esp_err_t device_state_set_ambient_light(uint8_t level, float lux);

/**
 * @brief Update key/touch state.
 */
esp_err_t device_state_set_key(uint8_t point, uint8_t event, uint32_t event_time_ms, uint8_t active_points);

#ifdef __cplusplus
}
#endif

#endif /* __DEVICE_STATE_H__ */
