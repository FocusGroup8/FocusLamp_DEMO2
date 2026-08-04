/*
 * status_packet.c - Status packet implementation
 */

#include <string.h>
#include "status_packet.h"

/* ===================== Power Status ===================== */
int status_pack_power(uint8_t *buf, size_t buf_size, const power_status_t *status)
{
    if (buf == NULL || status == NULL || buf_size < sizeof(power_status_t)) {
        return -2;
    }
    memcpy(buf, status, sizeof(power_status_t));
    return sizeof(power_status_t);
}

int status_unpack_power(const uint8_t *buf, size_t len, power_status_t *status)
{
    if (buf == NULL || status == NULL || len < sizeof(power_status_t)) {
        return -2;
    }
    memcpy(status, buf, sizeof(power_status_t));
    return 0;
}

/* ===================== Light Status ===================== */
int status_pack_light(uint8_t *buf, size_t buf_size, const light_status_t *status)
{
    if (buf == NULL || status == NULL || buf_size < sizeof(light_status_t)) {
        return -2;
    }
    memcpy(buf, status, sizeof(light_status_t));
    return sizeof(light_status_t);
}

int status_unpack_light(const uint8_t *buf, size_t len, light_status_t *status)
{
    if (buf == NULL || status == NULL || len < sizeof(light_status_t)) {
        return -2;
    }
    memcpy(status, buf, sizeof(light_status_t));
    return 0;
}

/* ===================== Servo Status ===================== */
int status_pack_servo(uint8_t *buf, size_t buf_size, const servo_status_t *status)
{
    if (buf == NULL || status == NULL || buf_size < sizeof(servo_status_t)) {
        return -2;
    }
    memcpy(buf, status, sizeof(servo_status_t));
    return sizeof(servo_status_t);
}

int status_unpack_servo(const uint8_t *buf, size_t len, servo_status_t *status)
{
    if (buf == NULL || status == NULL || len < sizeof(servo_status_t)) {
        return -2;
    }
    memcpy(status, buf, sizeof(servo_status_t));
    return 0;
}

/* ===================== Arm Status ===================== */
int status_pack_arm(uint8_t *buf, size_t buf_size, const arm_packed_status_t *status)
{
    if (buf == NULL || status == NULL || buf_size < sizeof(arm_packed_status_t)) {
        return -2;
    }
    memcpy(buf, status, sizeof(arm_packed_status_t));
    return sizeof(arm_packed_status_t);
}

int status_unpack_arm(const uint8_t *buf, size_t len, arm_packed_status_t *status)
{
    if (buf == NULL || status == NULL || len < sizeof(arm_packed_status_t)) {
        return -2;
    }
    memcpy(status, buf, sizeof(arm_packed_status_t));
    return 0;
}

/* ===================== Audio Status ===================== */
int status_pack_audio(uint8_t *buf, size_t buf_size, const audio_status_t *status)
{
    if (buf == NULL || status == NULL || buf_size < sizeof(audio_status_t)) {
        return -2;
    }
    memcpy(buf, status, sizeof(audio_status_t));
    return sizeof(audio_status_t);
}

int status_unpack_audio(const uint8_t *buf, size_t len, audio_status_t *status)
{
    if (buf == NULL || status == NULL || len < sizeof(audio_status_t)) {
        return -2;
    }
    memcpy(status, buf, sizeof(audio_status_t));
    return 0;
}

/* ===================== Sensor Status ===================== */
int status_pack_sensor(uint8_t *buf, size_t buf_size, const sensor_status_t *status)
{
    if (buf == NULL || status == NULL || buf_size < sizeof(sensor_status_t)) {
        return -2;
    }
    memcpy(buf, status, sizeof(sensor_status_t));
    return sizeof(sensor_status_t);
}

int status_unpack_sensor(const uint8_t *buf, size_t len, sensor_status_t *status)
{
    if (buf == NULL || status == NULL || len < sizeof(sensor_status_t)) {
        return -2;
    }
    memcpy(status, buf, sizeof(sensor_status_t));
    return 0;
}

/* ===================== System Status ===================== */
int status_pack_system(uint8_t *buf, size_t buf_size, const system_status_t *status)
{
    if (buf == NULL || status == NULL || buf_size < sizeof(system_status_t)) {
        return -2;
    }
    memcpy(buf, status, sizeof(system_status_t));
    return sizeof(system_status_t);
}

int status_unpack_system(const uint8_t *buf, size_t len, system_status_t *status)
{
    if (buf == NULL || status == NULL || len < sizeof(system_status_t)) {
        return -2;
    }
    memcpy(status, buf, sizeof(system_status_t));
    return 0;
}