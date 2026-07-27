/*
 * radar_driver.h - Radar sensor driver for FocusLamp
 * Uses UART port and pins from project configuration.
 *
 * Provides:
 * - UART initialization, read, flush, event queue
 * - TF frame data decoding utilities
 */

#pragma once
#ifndef __RADAR_DRIVER_H__
#define __RADAR_DRIVER_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== Forward Declarations ===================== */
/* tf_frame_t is defined in tf_parser.h - include it before this header if using decode API */

/* ===================== Radar Sensor Data ===================== */
/** Radar sensor data structure (legacy) */
typedef struct {
    float heart_rate;          /* Heart rate (bpm) */
    float respiration_rate;    /* Respiration rate (breaths/min) */
    float distance;            /* Target distance (cm) */
    bool  target_detected;     /* Whether a human target is detected */
    uint8_t signal_quality;    /* Signal quality indicator (0-100) */
} radar_data_t;

/* ===================== UART Driver API ===================== */

/**
 * @brief Initialize radar UART driver.
 *        Configures the radar UART port and pins from project configuration.
 * @return esp_err_t
 */
esp_err_t radar_driver_init(void);

/**
 * @brief Deinitialize radar UART driver.
 * @return esp_err_t
 */
esp_err_t radar_driver_deinit(void);

/**
 * @brief Get the UART event queue handle.
 * @return QueueHandle_t or NULL if not initialized
 */
QueueHandle_t radar_driver_get_event_queue(void);

/**
 * @brief Read bytes from radar UART.
 * @param buffer       Output buffer
 * @param length       Number of bytes to read
 * @param ticks_to_wait Max blocking time
 * @return Number of bytes read, or -1 on error
 */
int radar_driver_read_bytes(uint8_t *buffer, size_t length, TickType_t ticks_to_wait);

/**
 * @brief Flush radar UART input buffer.
 * @return esp_err_t
 */
esp_err_t radar_driver_flush_input(void);

/**
 * @brief Send a command to the radar module.
 * @param cmd  Pointer to command buffer
 * @param len  Command length in bytes
 * @return esp_err_t
 */
esp_err_t radar_driver_send_command(uint8_t *cmd, uint16_t len);

/**
 * @brief Read raw data from the radar module (legacy compatibility).
 * @param buffer   Pointer to receive buffer
 * @param max_len  Maximum buffer size
 * @return int     Number of bytes read, or -1 on error
 */
int radar_driver_read_data(uint8_t *buffer, uint16_t max_len);

/**
 * @brief Parse raw radar data frame into structured radar_data_t (legacy).
 * @param raw     Pointer to raw data buffer
 * @param output  Pointer to radar_data_t structure to fill
 * @return esp_err_t
 */
esp_err_t radar_driver_parse_data(uint8_t *raw, radar_data_t *output);

/* ===================== TF Frame Decode Utilities ===================== */

/**
 * @brief Decode a boolean from the first byte of frame data.
 *
 * @param frame     Pointer to tf_frame_t (must be included via tf_parser.h)
 * @return true     frame != NULL && data_length > 0 && data[0] != 0
 * @return false    otherwise
 */
bool radar_driver_decode_bool(const void *frame);

/**
 * @brief Decode a little-endian float at a given offset in frame data.
 *
 * @param frame   Pointer to tf_frame_t
 * @param offset  Byte offset into data payload
 * @return float  Decoded value, or 0.0f on error
 */
float radar_driver_decode_float_at(const void *frame, size_t offset);

/**
 * @brief Decode a little-endian uint32 at a given offset in frame data.
 *
 * @param frame   Pointer to tf_frame_t
 * @param offset  Byte offset into data payload
 * @return uint32_t Decoded value, or 0 on error
 */
uint32_t radar_driver_decode_uint32_at(const void *frame, size_t offset);

/**
 * @brief Decode a little-endian int32 at a given offset in frame data.
 *
 * @param frame   Pointer to tf_frame_t
 * @param offset  Byte offset into data payload
 * @return int32_t Decoded value, or 0 on error
 */
int32_t radar_driver_decode_int32_at(const void *frame, size_t offset);

#ifdef __cplusplus
}
#endif

#endif /* __RADAR_DRIVER_H__ */
