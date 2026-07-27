/*
 * custom_rule_app.h - Custom mode rule engine for FocusLamp
 * 
 * Allows users to create custom automation rules:
 *   Trigger Condition → Response Action
 * 
 * Trigger types: time-based, sensor-based (radar, light), touch-based
 * Action types: arm movement, LED lighting, audio playback, LCD expression
 */

#pragma once
#ifndef __CUSTOM_RULE_APP_H__
#define __CUSTOM_RULE_APP_H__

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== Trigger Types ===================== */
typedef enum {
    CUSTOM_TRIGGER_TYPE_NONE      = 0,
    CUSTOM_TRIGGER_TYPE_TIME,               /* Time-based trigger */
    CUSTOM_TRIGGER_TYPE_TIMER_EXPIRED,      /* Countdown timer expired */
    CUSTOM_TRIGGER_TYPE_RADAR_PRESENCE,     /* Human presence detected */
    CUSTOM_TRIGGER_TYPE_RADAR_CLEAR,        /* Human left */
    CUSTOM_TRIGGER_TYPE_LIGHT_LEVEL,        /* Ambient light threshold */
    CUSTOM_TRIGGER_TYPE_TOUCH_SINGLE,       /* Single touch */
    CUSTOM_TRIGGER_TYPE_TOUCH_DOUBLE,       /* Double touch */
    CUSTOM_TRIGGER_TYPE_TOUCH_LONG_PRESS,   /* Long press */
    CUSTOM_TRIGGER_TYPE_MAX,
} custom_trigger_type_t;

/* ===================== Trigger Conditions ===================== */
typedef struct {
    custom_trigger_type_t type;     /* Type of trigger */
    union {
        struct {
            uint8_t  hour;          /* 0-23 */
            uint8_t  minute;        /* 0-59 */
            bool     repeat_daily;  /* Repeat every day */
        } time;
        struct {
            uint32_t duration_sec;  /* Timer duration in seconds */
        } timer_expired;
        struct {
            bool     present;       /* true=presence, false=clear */
        } radar;
        struct {
            uint8_t  threshold;     /* 0-5 light level threshold */
            bool     above;         /* true=above, false=below */
        } light_level;
        struct {
            uint8_t  touch_point;   /* A=0, B=1, C=2, D=3 */
        } touch;
    } config;
} custom_trigger_t;

/* ===================== Action Types ===================== */
typedef enum {
    CUSTOM_ACTION_TYPE_NONE        = 0,
    CUSTOM_ACTION_TYPE_ARM_MOVE,            /* Arm movement sequence */
    CUSTOM_ACTION_TYPE_LED_MODE,            /* Set LED mode/color/brightness */
    CUSTOM_ACTION_TYPE_LED_EFFECT,          /* Set LED effect */
    CUSTOM_ACTION_TYPE_LCD_EXPRESSION,      /* Set LCD expression */
    CUSTOM_ACTION_TYPE_AUDIO_PLAY,          /* Play audio file */
    CUSTOM_ACTION_TYPE_AUDIO_TONE,          /* Play tone */
    CUSTOM_ACTION_TYPE_DELAY,               /* Delay between actions */
    CUSTOM_ACTION_TYPE_MAX,
} custom_action_type_t;

/* ===================== Action Definitions ===================== */
typedef struct {
    custom_action_type_t type;      /* Type of action */
    union {
        struct {
            uint8_t  servo_id;
            uint16_t position;
            uint32_t duration_ms;
        } arm_move;
        struct {
            uint8_t  mode;          /* led_mode_t */
            uint8_t  brightness;    /* 0-255 */
            uint8_t  r, g, b;       /* Color */
        } led;
        struct {
            uint8_t  effect;        /* led_effect_t */
        } led_effect;
        struct {
            uint8_t  expression;    /* lcd_expression_t */
        } lcd;
        struct {
            char     uri[128];      /* Audio file URI */
        } audio_play;
        struct {
            uint16_t freq_hz;
            uint32_t duration_ms;
        } audio_tone;
        struct {
            uint32_t delay_ms;
        } delay;
    } config;
} custom_action_t;

/* ===================== Rule Definition ===================== */
#define CUSTOM_RULE_NAME_MAX     32
#define CUSTOM_RULE_MAX_ACTIONS   8

typedef struct {
    char               name[CUSTOM_RULE_NAME_MAX];   /* Rule name */
    bool               enabled;                       /* Rule enabled */
    custom_trigger_t   trigger;                       /* Trigger condition */
    custom_action_t    actions[CUSTOM_RULE_MAX_ACTIONS]; /* Action list */
    uint8_t            action_count;                  /* Number of actions */
    uint32_t           cooldown_sec;                  /* Min seconds between triggers */
} custom_rule_t;

/* ===================== Rule Slots ===================== */
#define CUSTOM_RULE_MAX_SLOTS     8  /* Up to 8 rules stored in NVS */

/**
 * @brief Initialize custom rule app.
 *        Subscribes to EV_APP_MODE_CHANGED and related events.
 *        Loads rules from NVS.
 * @return esp_err_t
 */
esp_err_t custom_rule_app_init(void);

/**
 * @brief Start custom mode - begins monitoring triggers.
 * @return esp_err_t
 */
esp_err_t custom_rule_app_start(void);

/**
 * @brief Stop custom mode.
 * @return esp_err_t
 */
esp_err_t custom_rule_app_stop(void);

/**
 * @brief Add or update a rule at the specified slot.
 * @param slot  Slot index (0 ~ CUSTOM_RULE_MAX_SLOTS-1)
 * @param rule  Rule definition to save
 * @return esp_err_t
 */
esp_err_t custom_rule_app_set_rule(uint8_t slot, const custom_rule_t *rule);

/**
 * @brief Get a rule from the specified slot.
 * @param slot  Slot index (0 ~ CUSTOM_RULE_MAX_SLOTS-1)
 * @param[out] rule  Rule definition output
 * @return esp_err_t
 */
esp_err_t custom_rule_app_get_rule(uint8_t slot, custom_rule_t *rule);

/**
 * @brief Delete a rule from the specified slot.
 * @param slot  Slot index (0 ~ CUSTOM_RULE_MAX_SLOTS-1)
 * @return esp_err_t
 */
esp_err_t custom_rule_app_delete_rule(uint8_t slot);

/**
 * @brief Enable or disable a rule.
 * @param slot    Slot index
 * @param enable  true = enable, false = disable
 * @return esp_err_t
 */
esp_err_t custom_rule_app_set_rule_enabled(uint8_t slot, bool enable);

/**
 * @brief Get total number of active (enabled) rules.
 * @return uint8_t  Count of enabled rules
 */
uint8_t custom_rule_app_get_active_rule_count(void);

#ifdef __cplusplus
}
#endif

#endif /* __CUSTOM_RULE_APP_H__ */
