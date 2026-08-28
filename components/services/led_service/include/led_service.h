/*
 * led_service.h - Enhanced LED lighting service for FocusLamp
 *
 * Integrates:
 * - LED effects (breath, blink, heartbeat, system state, sedentary)
 * - Presence sensing (auto-off, distance-based)
 * - Light control (auto brightness via UART)
 */

#pragma once
#ifndef __LED_SERVICE_H__
#define __LED_SERVICE_H__

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#include "led_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== LED Effect Enumeration ===================== */
typedef enum {
    LED_EFFECT_STEADY      = 0,   /* 常亮 */
    LED_EFFECT_BREATHING,         /* 呼吸 */
    LED_EFFECT_RAINBOW,           /* 彩虹 */
    LED_EFFECT_BLINKING,          /* 闪烁 */
    LED_EFFECT_MUSIC_RHYTHM,      /* 音乐律动 */
} led_effect_t;

/* ===================== LED Mode Enumeration ===================== */
typedef enum {
    LED_MODE_WHITE  = 0,   /* 白光 */
    LED_MODE_WARM,         /* 暖光 */
    LED_MODE_COLOR,        /* 彩光 */
    LED_MODE_AMBIENT,      /* 氛围 */
} led_mode_t;

/* ===================== Brightness Level Definitions ===================== */
#define LED_BRIGHTNESS_LEVEL_MIN    0
#define LED_BRIGHTNESS_LEVEL_MAX    5
#define LED_BRIGHTNESS_LEVEL_COUNT  6

/* ===================== Common Color Presets ===================== */
#define LED_PRESET_COLOR_COUNT      5

typedef enum {
    LED_PRESET_WARM_WHITE   = 0,   /* 暖白 (255, 180, 80) */
    LED_PRESET_COOL_WHITE   = 1,   /* 冷白 (200, 220, 255) */
    LED_PRESET_RED          = 2,   /* 红色 (255, 0, 0) */
    LED_PRESET_GREEN        = 3,   /* 绿色 (0, 255, 0) */
    LED_PRESET_BLUE         = 4,   /* 蓝色 (0, 0, 255) */
} led_preset_color_t;

/* ===================== Public API ===================== */

/**
 * @brief Initialize LED service.
 *        Subscribes to EV_LIGHT_* events and initializes led_driver.
 * @return esp_err_t
 */
esp_err_t led_service_init(void);

/**
 * @brief Deinitialize LED service and release resources.
 * @return esp_err_t
 */
esp_err_t led_service_deinit(void);

/**
 * @brief Set LED color.
 * @param r  Red component (0-255)
 * @param g  Green component (0-255)
 * @param b  Blue component (0-255)
 * @return esp_err_t
 */
esp_err_t led_service_set_color(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Set global brightness.
 * @param brightness  Brightness level (0-255)
 * @return esp_err_t
 */
esp_err_t led_service_set_brightness(uint8_t brightness);

/**
 * @brief Set brightness level (0-5).
 *        0 = off, 5 = max.
 * @param level  Brightness level (0-5)
 * @return esp_err_t
 */
esp_err_t led_service_set_brightness_level(uint8_t level);

/**
 * @brief Map ambient light level (0-4) to LED brightness level (1-5).
 *        Same-direction one-to-one mapping: dark → low, bright → high.
 * @param ambient_level  Ambient light level (0-4)
 * @return uint8_t LED brightness level (1-5)
 */
uint8_t led_service_level_from_ambient(uint8_t ambient_level);

/**
 * @brief Get current brightness level (0-5).
 * @return uint8_t Current brightness level
 */
uint8_t led_service_get_brightness_level(void);

/**
 * @brief Increase brightness by one level.
 * @return esp_err_t
 */
esp_err_t led_service_brightness_up(void);

/**
 * @brief Decrease brightness by one level.
 * @return esp_err_t
 */
esp_err_t led_service_brightness_down(void);

/**
 * @brief Set LED lighting effect.
 * @param effect  Effect to apply
 * @return esp_err_t
 */
esp_err_t led_service_set_effect(led_effect_t effect);

/**
 * @brief Set LED lighting mode.
 * @param mode  Lighting mode to set
 * @return esp_err_t
 */
esp_err_t led_service_set_mode(led_mode_t mode);

/**
 * @brief Set LED to one of the 5 common color presets.
 * @param preset  Preset color index (LED_PRESET_WARM_WHITE ~ LED_PRESET_BLUE)
 * @return esp_err_t
 */
esp_err_t led_service_set_preset_color(led_preset_color_t preset);

/**
 * @brief Periodic update function for animated effects.
 *        Should be called from a task or timer at regular intervals.
 * @param dt_ms  Time delta since last update in milliseconds
 */
void led_service_update(uint32_t dt_ms);

/**
 * @brief Turn off all LEDs.
 * @return esp_err_t
 */
esp_err_t led_service_turn_off(void);

/**
 * @brief Show breathing effect.
 * @param elapsed_ms  Elapsed time in milliseconds for animation phase
 * @return esp_err_t
 */
esp_err_t led_service_show_breath(uint32_t elapsed_ms);

/**
 * @brief Show blinking effect.
 * @param mode       Blink mode (NONE/FAST/SLOW)
 * @param elapsed_ms Elapsed time in milliseconds
 * @return esp_err_t
 */
esp_err_t led_service_show_blink(led_blink_mode_t mode, uint32_t elapsed_ms);

/**
 * @brief Show presence indication.
 * @param is_human_present  true if human detected
 * @return esp_err_t
 */
esp_err_t led_service_show_presence(bool is_human_present);

/**
 * @brief Show heartbeat level.
 * @param level  Heartbeat level (0-255)
 * @return esp_err_t
 */
esp_err_t led_service_show_heartbeat_level(uint8_t level);

/**
 * @brief Show sedentary state based on radar motion.
 * @param state  Radar motion state
 * @return esp_err_t
 */
esp_err_t led_service_show_sedentary_state(radar_motion_state_t state);

/**
 * @brief Show system state indication.
 * @param state  System state
 * @return esp_err_t
 */
esp_err_t led_service_show_system_state(led_module_system_state_t state);

/**
 * @brief Get current LED service snapshot.
 * @param[out] snapshot  Snapshot structure to fill
 * @return esp_err_t
 */
esp_err_t led_service_get_snapshot(led_module_snapshot_t* snapshot);

/**
 * @brief Trigger activity to reset auto-off timer.
 * @return esp_err_t
 */
esp_err_t led_service_trigger_activity(void);

/**
 * @brief Update auto-off state machine.
 * @param current_time_ms  Current time in milliseconds
 * @return esp_err_t
 */
esp_err_t led_service_update_auto_off(uint32_t current_time_ms);

/**
 * @brief Update presence distance for distance-based effects.
 * @param distance_cm  Distance in centimeters
 * @return esp_err_t
 */
esp_err_t led_service_update_presence_distance(float distance_cm);

#ifdef __cplusplus
}
#endif

#endif /* __LED_SERVICE_H__ */
