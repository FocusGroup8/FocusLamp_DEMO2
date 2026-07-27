/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "status_receiver_config.h"
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#if (STATUS_RECEIVER_ENABLE == 1)

/* ===================== Status Data Structures ===================== */

/**
 * @brief LED status data received from FocusLamp
 */
typedef struct {
    bool     on;
    uint8_t  brightness;
    uint8_t  mode;
    uint8_t  r;
    uint8_t  g;
    uint8_t  b;
} status_led_t;

/**
 * @brief Display status data
 */
typedef struct {
    bool     on;
    uint8_t  brightness;
} status_display_t;

/**
 * @brief LCD status data
 */
typedef struct {
    uint8_t  page;
    char     expression[16];
    bool     auto_blink;
} status_lcd_t;

/**
 * @brief Servo status data
 */
typedef struct {
    int16_t  em3_pos;
    int16_t  lx_pos[4];
} status_servo_t;

/**
 * @brief Radar status data
 */
typedef struct {
    bool     present;
    float    heart_rate_bpm;
    float    breath_rate_bpm;
    float    distance_cm;
} status_radar_t;

/**
 * @brief Ambient light status data
 */
typedef struct {
    uint8_t  level;
    float    lux;
} status_ambient_light_t;

/**
 * @brief System status data
 */
typedef struct {
    uint32_t free_heap;
    int      wifi_rssi;
    uint32_t uptime;
} status_system_t;

/**
 * @brief Complete device status snapshot
 */
typedef struct {
    bool                   valid;       /* True if data has been received */
    uint32_t               timestamp;   /* Reception time (millis) */
    char                   device[16];  /* Device name */
    char                   version[16]; /* Firmware version */
    status_led_t           led;
    status_display_t       display;
    status_lcd_t           lcd;
    status_servo_t         servo;
    status_radar_t         radar;
    status_ambient_light_t ambient_light;
    status_system_t        system;
} status_receiver_data_t;

/* ===================== Public API ===================== */

/**
 * @brief Initialize status receiver and register HTTP endpoint
 *
 * Registers POST /api/status/report handler on the WebSocket manager's
 * HTTP server instance.
 *
 * @return ESP_OK on success, error code on failure
 */
esp_err_t status_receiver_init(void);

/**
 * @brief Deinitialize status receiver
 */
void status_receiver_deinit(void);

/**
 * @brief Get current status snapshot
 *
 * Returns a copy of the most recently received status data.
 *
 * @param[out] data  Pointer to status_receiver_data_t to fill
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no data received yet
 */
esp_err_t status_receiver_get_status(status_receiver_data_t *data);

/**
 * @brief Get status as JSON string
 *
 * Generates a JSON string from the current status data.
 *
 * @param[out] buf      Output buffer
 * @param[in]  buf_size Buffer size
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t status_receiver_get_json(char *buf, int buf_size);

/**
 * @brief Check if status data is fresh
 *
 * @return true if data received within STATUS_RECEIVER_STALE_TIMEOUT_MS
 */
bool status_receiver_is_fresh(void);

#else /* STATUS_RECEIVER_ENABLE == 0 */

/* Stub implementations when disabled */
static inline esp_err_t status_receiver_init(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline void status_receiver_deinit(void) {}
static inline esp_err_t status_receiver_get_status(status_receiver_data_t *data) { (void)data; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t status_receiver_get_json(char *buf, int buf_size) { (void)buf; (void)buf_size; return ESP_ERR_NOT_SUPPORTED; }
static inline bool status_receiver_is_fresh(void) { return false; }

#endif /* STATUS_RECEIVER_ENABLE */

#ifdef __cplusplus
}
#endif
