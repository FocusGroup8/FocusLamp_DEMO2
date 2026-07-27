/*
 * command_parser.h - Command parser definitions
 */

#pragma once
#ifndef __COMMAND_PARSER_H__
#define __COMMAND_PARSER_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== Command Types ===================== */
typedef enum {
    /* === Power Commands === */
    CMD_POWER_ON                = 0x0101,
    CMD_POWER_OFF               = 0x0102,
    CMD_POWER_RESET             = 0x0103,
    CMD_POWER_STATUS_REQ        = 0x0104,

    /* === Light Commands === */
    CMD_LIGHT_SET_BRIGHTNESS    = 0x0201,
    CMD_LIGHT_SET_COLOR_TEMP    = 0x0202,
    CMD_LIGHT_SET_COLOR_RGB     = 0x0203,
    CMD_LIGHT_SET_MODE          = 0x0204,
    CMD_LIGHT_TOGGLE            = 0x0205,

    /* === Servo Commands === */
    CMD_SERVO_SET_POSITION      = 0x0301,
    CMD_SERVO_SET_SPEED         = 0x0302,
    CMD_SERVO_STOP              = 0x0303,
    CMD_SERVO_STATUS_REQ        = 0x0304,

    /* === Arm Commands === */
    CMD_ARM_START_SEQUENCE      = 0x0401,
    CMD_ARM_STOP_SEQUENCE       = 0x0402,
    CMD_ARM_PAUSE_SEQUENCE      = 0x0403,
    CMD_ARM_EMERGENCY_STOP      = 0x0404,
    CMD_ARM_SET_SPEED           = 0x0405,

    /* === Audio Commands === */
    CMD_AUDIO_PLAY              = 0x0501,
    CMD_AUDIO_STOP              = 0x0502,
    CMD_AUDIO_SET_VOLUME        = 0x0503,
    CMD_AUDIO_NEXT_TRACK        = 0x0504,

    /* === System Commands === */
    CMD_SYS_GET_STATUS          = 0x0601,
    CMD_SYS_SET_MODE            = 0x0602,
    CMD_SYS_HEARTBEAT           = 0x0603,
    CMD_SYS_SYNC_TIME           = 0x0604,
    CMD_SYS_VERSION_REQ         = 0x0605,

} command_type_t;

/* ===================== Command Structure ===================== */
typedef struct {
    command_type_t  cmd_type;
    uint8_t         sequence;
    uint8_t        *params;
    uint8_t         param_len;
} command_t;

/* ===================== Command Event Data ===================== */
#define COMMAND_EVENT_MAX_PARAM_LEN 64

typedef struct {
    command_type_t  cmd_type;
    uint8_t         sequence;
    uint8_t         param_len;
    uint8_t         params[COMMAND_EVENT_MAX_PARAM_LEN];
} command_event_data_t;

/* ===================== Function Prototypes ===================== */
int command_parse(const uint8_t *data, uint8_t len, command_t *cmd);
int command_dispatch(const command_t *cmd);

#ifdef __cplusplus
}
#endif

#endif /* __COMMAND_PARSER_H__ */