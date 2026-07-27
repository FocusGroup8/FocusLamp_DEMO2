/*
 * error_code.h - Error code definitions for FocusLamp
 */

#pragma once
#ifndef __ERROR_CODE_H__
#define __ERROR_CODE_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /* === General === */
    ERR_OK                          = 0,        /* Success */
    ERR_FAIL                        = -1,       /* General failure */
    ERR_INVALID_PARAM               = -2,       /* Invalid parameter */
    ERR_TIMEOUT                     = -3,       /* Operation timeout */
    ERR_NOT_INITIALIZED             = -4,       /* Module not initialized */
    ERR_ALREADY_INITIALIZED         = -5,       /* Module already initialized */
    ERR_NO_MEM                      = -6,       /* Memory allocation failed */
    ERR_BUSY                        = -7,       /* Resource busy */
    ERR_NOT_FOUND                   = -8,       /* Not found */
    ERR_UNSUPPORTED                 = -9,       /* Unsupported operation */

    /* === Power Management === */
    ERR_POWER_BASE                  = -100,
    ERR_POWER_LOW_BATTERY           = -101,
    ERR_POWER_OVER_CURRENT          = -102,
    ERR_POWER_OVER_VOLTAGE          = -103,
    ERR_POWER_UNDER_VOLTAGE         = -104,
    ERR_POWER_CHARGE_FAIL           = -105,

    /* === Touch === */
    ERR_TOUCH_BASE                  = -200,
    ERR_TOUCH_CALIBRATION_FAIL      = -201,
    ERR_TOUCH_NOISE                 = -202,
    ERR_TOUCH_OVERFLOW              = -203,

    /* === LCD === */
    ERR_LCD_BASE                    = -300,
    ERR_LCD_INIT_FAIL               = -301,
    ERR_LCD_SPI_FAIL                = -302,
    ERR_LCD_REFRESH_TIMEOUT         = -303,

    /* === Audio === */
    ERR_AUDIO_BASE                  = -400,
    ERR_AUDIO_I2S_INIT_FAIL         = -401,
    ERR_AUDIO_CODEC_NOT_FOUND       = -402,
    ERR_AUDIO_PLAYBACK_FAIL         = -403,
    ERR_AUDIO_FILE_NOT_FOUND        = -404,
    ERR_AUDIO_DECODE_FAIL           = -405,

    /* === Servo === */
    ERR_SERVO_BASE                  = -500,
    ERR_SERVO_UART_FAIL             = -501,
    ERR_SERVO_POSITION_OUT_RANGE    = -502,
    ERR_SERVO_SPEED_OUT_RANGE       = -503,
    ERR_SERVO_NO_RESPONSE           = -504,
    ERR_SERVO_OVERLOAD              = -505,

    /* === Mechanical Arm === */
    ERR_ARM_BASE                    = -600,
    ERR_ARM_SEQUENCE_INVALID        = -601,
    ERR_ARM_EMERGENCY_STOP          = -602,
    ERR_ARM_POSITION_TIMEOUT        = -603,
    ERR_ARM_COLLISION_DETECTED      = -604,

    /* === Sensor === */
    ERR_SENSOR_BASE                 = -700,
    ERR_SENSOR_AMBIENT_LIGHT_FAIL   = -701,
    ERR_SENSOR_RADAR_FAIL           = -702,
    ERR_SENSOR_HEART_RATE_FAIL      = -703,
    ERR_SENSOR_CALIBRATION_FAIL     = -704,

    /* === Communication === */
    ERR_COMM_BASE                   = -800,
    ERR_COMM_UART_INIT_FAIL         = -801,
    ERR_COMM_FRAME_ERROR            = -802,
    ERR_COMM_CHECKSUM_FAIL          = -803,
    ERR_COMM_TIMEOUT                = -804,
    ERR_COMM_BUFFER_OVERFLOW        = -805,
    ERR_COMM_PROTOCOL_ERROR         = -806,

    /* === Application === */
    ERR_APP_BASE                    = -900,
    ERR_APP_MODE_INVALID            = -901,
    ERR_APP_STATE_INVALID           = -902,
    ERR_APP_TRANSITION_FAIL         = -903,

} error_code_t;

#ifdef __cplusplus
}
#endif

#endif /* __ERROR_CODE_H__ */