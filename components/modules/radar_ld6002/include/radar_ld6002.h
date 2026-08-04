/*
 * radar_ld6002.h - Self-contained HLK-LD6002 radar reader
 *
 * Migrated from the standalone radar test (radar/main/main.c).
 * Parses TF frames (SOF=0x01, big-endian fields, XOR-NOT checksum)
 * from the HLK-LD6002 radar module via the shared radar_driver UART
 * layer, decodes them into a radar state, publishes EV_RADAR_* events
 * onto the event_bus (so device_state / focus_app / lcd_service stay in
 * sync), and periodically prints a summary.
 */

#pragma once
#ifndef __RADAR_LD6002_H__
#define __RADAR_LD6002_H__

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== Radar State ===================== */
/* Mirrors the standalone test's radar_state_t. */
typedef struct {
    float    heart_rate;      /* bpm */
    float    breath_rate;     /* breaths/min */
    float    total_phase;
    float    breath_phase;
    float    heart_phase;
    float    pos_x;
    float    pos_y;
    float    pos_z;
    float    range_dist;      /* raw distance value from module */
    uint32_t target_count;
    uint32_t range_flag;
    bool     person_present;
    bool     has_heart;
    bool     has_breath;
    bool     has_phase;
    bool     has_pos;
    bool     has_range;
    bool     has_presence;
} radar_ld6002_state_t;

/* ===================== Parser Statistics ===================== */
typedef struct {
    uint32_t valid_frames;    /* frames that passed all checksums */
    uint32_t ck_errors;       /* data checksum failures */
    uint32_t sof_found;       /* start-of-frame bytes seen */
    uint32_t hdr_ck_fails;    /* header checksum failures */
} radar_ld6002_stats_t;

/* ===================== Public API ===================== */

/**
 * @brief Initialize the HLK-LD6002 radar reader.
 *        Initializes the shared radar_driver UART (idempotent) and
 *        resets the internal parser/state. Does not start the task.
 * @return ESP_OK on success, or an esp_err_t from radar_driver_init.
 */
esp_err_t radar_ld6002_init(void);

/**
 * @brief Deinitialize the reader and stop the task if running.
 */
esp_err_t radar_ld6002_deinit(void);

/**
 * @brief Start the background radar reader task.
 *        radar_ld6002_init() must be called first.
 * @return ESP_OK, ESP_ERR_INVALID_STATE, or ESP_FAIL.
 */
esp_err_t radar_ld6002_start(void);

/**
 * @brief Stop the background radar reader task.
 */
esp_err_t radar_ld6002_stop(void);

/**
 * @brief Whether the reader task is currently running.
 */
bool radar_ld6002_is_running(void);

/**
 * @brief Get a snapshot of the latest decoded radar state.
 */
esp_err_t radar_ld6002_get_state(radar_ld6002_state_t *state);

/**
 * @brief Get parser statistics.
 */
esp_err_t radar_ld6002_get_stats(radar_ld6002_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* __RADAR_LD6002_H__ */
