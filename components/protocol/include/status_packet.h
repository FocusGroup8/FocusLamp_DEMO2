/*
 * status_packet.h - Status packet definitions
 */

#pragma once
#ifndef __STATUS_PACKET_H__
#define __STATUS_PACKET_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== Status Types ===================== */
typedef enum {
    STATUS_POWER         = 0x01,
    STATUS_LIGHT         = 0x02,
    STATUS_SERVO         = 0x03,
    STATUS_ARM           = 0x04,
    STATUS_AUDIO         = 0x05,
    STATUS_SENSOR        = 0x06,
    STATUS_SYSTEM        = 0x07,
} status_type_t;

/* ===================== Status Structures ===================== */
typedef struct __attribute__((packed)) {
    uint8_t     battery_level;      /* 0-100 % */
    uint8_t     is_charging;
    uint8_t     power_state;
} power_status_t;

typedef struct __attribute__((packed)) {
    uint8_t     brightness;         /* 0-255 */
    uint16_t    color_temp;         /* Kelvin */
    uint8_t     mode;
    uint8_t     is_on;
} light_status_t;

typedef struct __attribute__((packed)) {
    uint8_t     servo_id;
    uint16_t    current_position;
    uint16_t    target_position;
    uint8_t     speed;
    uint8_t     state;
} servo_status_t;

typedef struct __attribute__((packed)) {
    uint8_t     sequence_id;
    uint8_t     step_index;
    uint8_t     state;              /* running/paused/stopped */
    uint8_t     error_code;
} arm_packed_status_t;

typedef struct __attribute__((packed)) {
    uint8_t     state;              /* playing/stopped/paused */
    uint8_t     volume;             /* 0-100 */
    uint16_t    track_index;
} audio_status_t;

typedef struct __attribute__((packed)) {
    uint16_t    ambient_light;      /* lux */
    uint8_t     radar_detected;
    uint8_t     heart_rate;
} sensor_status_t;

typedef struct __attribute__((packed)) {
    uint8_t     system_state;
    uint8_t     app_mode;
    uint32_t    uptime_ms;
    uint16_t    error_flags;
} system_status_t;

/* ===================== Function Prototypes ===================== */
int status_pack_power(uint8_t *buf, size_t buf_size, const power_status_t *status);
int status_unpack_power(const uint8_t *buf, size_t len, power_status_t *status);

int status_pack_light(uint8_t *buf, size_t buf_size, const light_status_t *status);
int status_unpack_light(const uint8_t *buf, size_t len, light_status_t *status);

int status_pack_servo(uint8_t *buf, size_t buf_size, const servo_status_t *status);
int status_unpack_servo(const uint8_t *buf, size_t len, servo_status_t *status);

int status_pack_arm(uint8_t *buf, size_t buf_size, const arm_packed_status_t *status);
int status_unpack_arm(const uint8_t *buf, size_t len, arm_packed_status_t *status);

int status_pack_audio(uint8_t *buf, size_t buf_size, const audio_status_t *status);
int status_unpack_audio(const uint8_t *buf, size_t len, audio_status_t *status);

int status_pack_sensor(uint8_t *buf, size_t buf_size, const sensor_status_t *status);
int status_unpack_sensor(const uint8_t *buf, size_t len, sensor_status_t *status);

int status_pack_system(uint8_t *buf, size_t buf_size, const system_status_t *status);
int status_unpack_system(const uint8_t *buf, size_t len, system_status_t *status);

#ifdef __cplusplus
}
#endif

#endif /* __STATUS_PACKET_H__ */