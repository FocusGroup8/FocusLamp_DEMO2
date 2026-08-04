/*
 * radar_module.h - Radar module interface
 *
 * Provides:
 * - Radar data reception and processing via UART
 * - TF frame parsing
 * - Error detection and recovery
 * - Status monitoring
 */

#pragma once
#ifndef __RADAR_MODULE_H__
#define __RADAR_MODULE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "driver/uart.h"
#include "led_types.h"
#include "project_config.h"
#include "radar_module_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== TF Message Types ===================== */
#ifndef RADAR_MSG_TYPE_HUMAN_DETECT
#define RADAR_MSG_TYPE_HUMAN_DETECT     0x0001
#endif
#ifndef RADAR_MSG_TYPE_PHASE_TEST
#define RADAR_MSG_TYPE_PHASE_TEST       0x0004
#endif
#ifndef RADAR_MSG_TYPE_HEART_RATE
#define RADAR_MSG_TYPE_HEART_RATE       0x0003
#endif
#ifndef RADAR_MSG_TYPE_BREATH_RATE
#define RADAR_MSG_TYPE_BREATH_RATE      0x0002
#endif
#ifndef RADAR_MSG_TYPE_POSITION
#define RADAR_MSG_TYPE_POSITION         0x0005
#endif
#ifndef RADAR_MSG_TYPE_TARGET_RANGE
#define RADAR_MSG_TYPE_TARGET_RANGE     0x0006
#endif
#ifndef RADAR_MSG_TYPE_TARGET_TRACK
#define RADAR_MSG_TYPE_TARGET_TRACK     0x0007
#endif
#ifndef RADAR_MSG_TYPE_FIRMWARE_STATUS
#define RADAR_MSG_TYPE_FIRMWARE_STATUS  0x0008
#endif

/* ===================== TF Frame Layout ===================== */
#define RADAR_MODULE_TF_HEADER_SIZE          8
#define RADAR_MODULE_TF_MAX_PAYLOAD_SIZE     128
#define RADAR_MODULE_TF_MAX_FRAME_SIZE       (RADAR_MODULE_TF_HEADER_SIZE + RADAR_MODULE_TF_MAX_PAYLOAD_SIZE + 1)

typedef struct tf_frame {
    uint16_t frame_id;
    uint16_t data_length;
    uint16_t message_type;
    uint8_t  header_checksum;
    uint8_t  data[RADAR_MODULE_TF_MAX_PAYLOAD_SIZE];
    uint8_t  data_checksum;
    size_t   total_length;
} tf_frame_t;

/* ===================== Radar Event Data Types ===================== */
typedef struct {
    bool     is_present;
    float    distance_cm;
    float    x;
    float    y;
    float    z;
} radar_presence_data_t;

typedef struct {
    bool     is_detected;
    float    breath_phase;
    float    breath_rate;
} radar_breath_data_t;

typedef struct {
    bool     is_valid;
    float    heart_rate;
    float    heart_phase;
} radar_heart_data_t;

typedef struct {
    radar_motion_state_t state;
    int64_t              duration_ms;
} radar_motion_data_t;

typedef struct {
    float    x;
    float    y;
    float    z;
    int32_t  dop_idx;
    int32_t  cluster_id;
} radar_position_data_t;

typedef struct {
    uint32_t flag;
    float    range_cm;
} radar_target_range_data_t;

typedef struct {
    float    x;
    float    y;
    float    z;
} radar_target_track_data_t;

typedef struct {
    float    sdnn_ms;
    float    rmssd_ms;
    float    mean_hr_bpm;
    bool     valid;
} radar_hrv_data_t;

/* ===================== Status ===================== */
typedef enum {
    RADAR_MODULE_STATUS_IDLE = 0,
    RADAR_MODULE_STATUS_RUNNING,
    RADAR_MODULE_STATUS_FRAME_READY,
    RADAR_MODULE_STATUS_ERROR,
} radar_module_status_t;

/* Parser result codes (mirrored from tf_parser.h to avoid hard dependency) */
typedef enum {
    RADAR_PARSER_OK = 0,
    RADAR_PARSER_INCOMPLETE,
    RADAR_PARSER_INVALID_ARG,
    RADAR_PARSER_INVALID_LENGTH,
    RADAR_PARSER_HEADER_CHECKSUM_ERROR,
    RADAR_PARSER_DATA_CHECKSUM_ERROR,
    RADAR_PARSER_BUFFER_OVERFLOW,
} radar_parser_result_t;

/* ===================== Snapshot ===================== */
typedef struct {
    radar_module_status_t status;
    radar_parser_result_t last_parser_result;
    tf_frame_t            last_frame;
    size_t                total_bytes_processed;
    size_t                total_frames_parsed;
    size_t                total_parser_errors;
    size_t                consecutive_errors;
    int64_t               last_receive_time_us;
    bool                  has_new_frame;
} radar_module_snapshot_t;

/* ===================== Public API ===================== */

esp_err_t radar_module_init(void);
esp_err_t radar_module_deinit(void);
esp_err_t radar_module_start(void);
esp_err_t radar_module_stop(void);
bool      radar_module_is_running(void);

QueueHandle_t radar_module_get_event_queue(void);
QueueHandle_t radar_module_get_frame_queue(void);

esp_err_t radar_module_handle_uart_event(const uart_event_t *event);
esp_err_t radar_module_poll(size_t max_bytes_to_process, TickType_t ticks_to_wait);

esp_err_t radar_module_get_snapshot(radar_module_snapshot_t *snapshot);
esp_err_t radar_module_clear_new_frame_flag(void);
bool      radar_module_is_initialized(void);
esp_err_t radar_module_reset_parser(void);
bool      radar_module_is_in_error_state(void);
bool      radar_module_is_timeout(void);
int64_t   radar_module_get_time_since_last_receive_ms(void);

esp_err_t radar_module_get_resource_status(char *buffer, size_t buffer_size);

void radar_module_enable_log(void);
void radar_module_disable_log(void);
bool radar_module_is_log_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* __RADAR_MODULE_H__ */
