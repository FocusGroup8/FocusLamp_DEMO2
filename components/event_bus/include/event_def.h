/*
 * event_def.h - Event type definitions for FocusLamp event bus
 */

#pragma once
#ifndef __EVENT_DEF_H__
#define __EVENT_DEF_H__

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== Event Types ===================== */
typedef enum {
    /* === System Events === */
    EV_SYS_STARTUP_COMPLETE         = 0x0100,
    EV_SYS_ERROR,
    EV_SYS_WATCHDOG_TRIGGERED,
    EV_SYS_STATE_CHANGED,
    EV_SYS_SHUTDOWN,

    /* === Power Events === */
    EV_POWER_ON                     = 0x0200,
    EV_POWER_OFF,
    EV_POWER_LOW_BATTERY,
    EV_POWER_CHARGING,
    EV_POWER_CHARGED,
    EV_POWER_OVER_CURRENT,

    /* === Touch Events - Point A === */
    EV_TOUCH_A_SINGLE_CLICK         = 0x0300,
    EV_TOUCH_A_DOUBLE_CLICK,
    EV_TOUCH_A_LONG_PRESS,
    EV_TOUCH_A_SLIDE_UP,
    EV_TOUCH_A_SLIDE_DOWN,
    EV_TOUCH_A_SLIDE_LEFT,
    EV_TOUCH_A_SLIDE_RIGHT,
    EV_TOUCH_A_RELEASE,

    /* === Touch Events - Point B === */
    EV_TOUCH_B_SINGLE_CLICK         = 0x0310,
    EV_TOUCH_B_DOUBLE_CLICK,
    EV_TOUCH_B_LONG_PRESS,
    EV_TOUCH_B_SLIDE_UP,
    EV_TOUCH_B_SLIDE_DOWN,
    EV_TOUCH_B_SLIDE_LEFT,
    EV_TOUCH_B_SLIDE_RIGHT,
    EV_TOUCH_B_RELEASE,

    /* === Touch Events - Point C === */
    EV_TOUCH_C_SINGLE_CLICK         = 0x0320,
    EV_TOUCH_C_DOUBLE_CLICK,
    EV_TOUCH_C_LONG_PRESS,
    EV_TOUCH_C_SLIDE_UP,
    EV_TOUCH_C_SLIDE_DOWN,
    EV_TOUCH_C_SLIDE_LEFT,
    EV_TOUCH_C_SLIDE_RIGHT,
    EV_TOUCH_C_RELEASE,

    /* === Touch Events - Point D === */
    EV_TOUCH_D_SINGLE_CLICK         = 0x0330,
    EV_TOUCH_D_DOUBLE_CLICK,
    EV_TOUCH_D_LONG_PRESS,
    EV_TOUCH_D_SLIDE_UP,
    EV_TOUCH_D_SLIDE_DOWN,
    EV_TOUCH_D_SLIDE_LEFT,
    EV_TOUCH_D_SLIDE_RIGHT,
    EV_TOUCH_D_RELEASE,

    /* === Touch Combo Events === */
    EV_TOUCH_AB_COMBO               = 0x0340,
    EV_TOUCH_AC_COMBO,
    EV_TOUCH_AD_COMBO,
    EV_TOUCH_BC_COMBO,
    EV_TOUCH_BD_COMBO,
    EV_TOUCH_CD_COMBO,

    /* === Light Events === */
    EV_LIGHT_BRIGHTNESS_UP          = 0x0400,
    EV_LIGHT_BRIGHTNESS_DOWN,
    EV_LIGHT_COLOR_TEMP_WARM,
    EV_LIGHT_COLOR_TEMP_COOL,
    EV_LIGHT_MODE_NORMAL,
    EV_LIGHT_MODE_FOCUS,
    EV_LIGHT_MODE_NIGHT,
    EV_LIGHT_TOGGLE,
    EV_LIGHT_PRESET_COLOR,

    /* === LCD Events === */
    EV_LCD_UPDATE                   = 0x0500,
    EV_LCD_PAGE_CHANGE,
    EV_LCD_SLEEP,
    EV_LCD_WAKEUP,
    EV_LCD_BRIGHTNESS_CHANGED,
    EV_LCD_TIMEOUT,

    /* === Audio Events === */
    EV_AUDIO_PLAY                   = 0x0600,
    EV_AUDIO_STOP,
    EV_AUDIO_PAUSE,
    EV_AUDIO_RESUME,
    EV_AUDIO_VOLUME_UP,
    EV_AUDIO_VOLUME_DOWN,
    EV_AUDIO_VOLUME_SET,
    EV_AUDIO_TRACK_CHANGE,
    EV_AUDIO_PLAYBACK_DONE,
    EV_AUDIO_ERROR,
    EV_AUDIO_STATE_CHANGED,

    /* === Servo Events === */
    EV_SERVO_MOVE                   = 0x0700,
    EV_SERVO_SPEED_SET,
    EV_SERVO_STOP,
    EV_SERVO_POSITION_REACHED,
    EV_SERVO_ERROR,
    EV_SERVO_STATE_CHANGED,

    /* === Mechanical Arm Events === */
    EV_ARM_SEQUENCE_START           = 0x0800,
    EV_ARM_SEQUENCE_STOP,
    EV_ARM_SEQUENCE_PAUSE,
    EV_ARM_EMERGENCY_STOP,
    EV_ARM_POSITION_REACHED,
    EV_ARM_SEQUENCE_DONE,
    EV_ARM_ERROR,

    /* === Sensor Events === */
    EV_SENSOR_AMBIENT_LIGHT_CHANGED = 0x0900,
    EV_SENSOR_RADAR_DETECTED,
    EV_SENSOR_RADAR_CLEAR,
    EV_SENSOR_HEART_RATE_DATA,
    EV_SENSOR_HEART_RATE_ALERT,

    /* === Communication Events === */
    EV_COMM_DATA_RECEIVED           = 0x0A00,
    EV_COMM_DATA_SENT,
    EV_COMM_CONNECTED,
    EV_COMM_DISCONNECTED,
    EV_COMM_ERROR,
    EV_COMM_FRAME_RECEIVED,
    EV_COMM_HEARTBEAT,
    EV_COMM_STATUS_RECEIVED,

    /* === Application Events === */
    EV_APP_MODE_CHANGED             = 0x0B00,
    EV_APP_STATE_CHANGED,
    EV_APP_FOCUS_TIMER_START,
    EV_APP_FOCUS_TIMER_TICK,
    EV_APP_FOCUS_TIMER_DONE,
    EV_APP_GAME_EVENT,

    /* === WiFi Comm Events (inter-device TCP communication) === */
    EV_WIFI_COMM_CONNECTED          = 0x0B10,
    EV_WIFI_COMM_DISCONNECTED,
    EV_WIFI_COMM_DATA_RECEIVED,
    EV_WIFI_COMM_DATA_SENT,
    EV_WIFI_COMM_ERROR,

    /* === Radar Events === */
    EV_RADAR_PRESENCE               = 0x0C00,
    EV_RADAR_BREATH,
    EV_RADAR_HEART_RATE,
    EV_RADAR_HEART_RATE_ENABLE,
    EV_RADAR_HEART_RATE_DISABLE,
    EV_RADAR_MOTION,
    EV_RADAR_POSITION,
    EV_RADAR_TARGET_RANGE,
    EV_RADAR_TARGET_TRACK,
    EV_RADAR_HRV_READY,

    /* === Network Events === */
    EV_NETWORK_CONNECTED            = 0x0D00,
    EV_NETWORK_DISCONNECTED,
    EV_NETWORK_IP_OBTAINED,
    EV_NETWORK_CONNECTION_FAILED,
    EV_NETWORK_RSSI_CHANGED,

    /* === Voice Events === */
    EV_VOICE_COMMAND_RECOGNIZED     = 0x0E00,
    EV_VOICE_WAKE_WORD_DETECTED,
    EV_VOICE_NO_MATCH,
    EV_VOICE_ERROR,

    /* === Command Events === */
    EV_COMMAND_RECEIVED             = 0x0F00,

} event_type_t;

/* ===================== Event Structure ===================== */
typedef struct {
    event_type_t    type;
    void           *data;
    size_t          data_size;
    uint32_t        timestamp;  /* ms since boot */
} event_t;

#ifdef __cplusplus
}
#endif

#endif /* __EVENT_DEF_H__ */