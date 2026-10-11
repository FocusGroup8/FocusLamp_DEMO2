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


#ifdef __cplusplus
}
#endif
#endif
