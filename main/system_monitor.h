/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// C++ compatibility
#ifdef __cplusplus
extern "C" {
#endif

// Forward declaration for FreeRTOS types
struct xTASK_STATUS;
typedef struct xTASK_STATUS TaskStatus_t;

/**
 * @brief System monitor configuration
 */
typedef struct {
    uint32_t interval_sec;     /*!< Logging interval in seconds */
    bool print_task_list;      /*!< Include FreeRTOS task list in report */
    bool enable_audio_metrics; /*!< Track audio-specific metrics */
} sysmon_config_t;

/**
 * @brief Audio-specific metrics structure
 */
typedef struct {
    size_t heap_before_audio;  /*!< Heap size before audio init (bytes) */
    size_t heap_after_audio;   /*!< Heap size after audio init (bytes) */
    uint32_t rec_duration_ms;  /*!< Total recording duration (ms) */
    uint32_t play_duration_ms; /*!< Total playback duration (ms) */
    int recorder_state;        /*!< Recorder state enum value */
    int player_state;          /*!< Player state enum value */
} sysmon_audio_metrics_t;

/**
 * @brief System resource report structure
 */
typedef struct {
    size_t total_heap;    /*!< Total heap size (bytes) */
    size_t free_heap;     /*!< Free heap size (bytes) */
    size_t min_free_heap; /*!< Minimum free heap ever (bytes) */
    size_t largest_block; /*!< Largest free block (bytes) */
#if CONFIG_SPIRAM
    size_t psram_total; /*!< PSRAM total size (bytes) */
    size_t psram_free;  /*!< PSRAM free size (bytes) */
#endif
    float heap_usage_pct; /*!< Heap usage percentage */

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    float cpu_usage_pct; /*!< Overall CPU usage percentage */
#endif
} sysmon_report_t;

/**
 * @brief Task-specific metrics structure
 */
typedef struct {
    char task_name[16];  /*!< Task name */
    uint32_t stack_hwm;  /*!< Stack high water mark (bytes) */
    float cpu_usage_pct; /*!< Task CPU usage percentage */
    uint8_t priority;    /*!< Task priority */
    int state;           /*!< Task state (eTaskState enum value) */
} sysmon_task_metrics_t;

/**
 * @brief Get task-specific metrics
 *
 * Retrieves statistics for specific tasks (audio-related).
 *
 * @param[out] metrics  Task metrics array
 * @param[in]  max_tasks Maximum number of tasks to retrieve
 * @return Number of tasks retrieved
 */
int sysmon_get_task_metrics(sysmon_task_metrics_t *metrics, int max_tasks);

/**
 * @brief Initialize system monitoring module
 *
 * Sets up periodic timer for automatic logging.
 * Can be called in both AUDIO and DISPLAY modes.
 *
 * @param config  Monitor configuration (NULL for defaults)
 * @return ESP_OK on success
 */
esp_err_t sysmon_init(const sysmon_config_t *config);

/**
 * @brief Deinitialize system monitoring module
 *
 * Stops timer and releases resources.
 */
void sysmon_deinit(void);

/**
 * @brief Start periodic system status logging
 *
 * Logs memory and task info at configured interval.
 *
 * @return ESP_OK on success
 */
esp_err_t sysmon_start(void);

/**
 * @brief Stop periodic logging
 */
void sysmon_stop(void);

/**
 * @brief Print current system status (one-time)
 *
 * Outputs to ESP_LOGI with tag "SYSMON"
 */
void sysmon_print_status(void);

/**
 * @brief Get current system resource report
 *
 * @param[out] report  System resource statistics
 * @return ESP_OK on success
 */
esp_err_t sysmon_get_report(sysmon_report_t *report);

/**
 * @brief Get audio-specific metrics
 *
 * Only available when EXAMPLE_ENABLE_AUDIO is enabled.
 *
 * @param[out] metrics  Audio subsystem statistics
 * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED if audio disabled
 */
esp_err_t sysmon_get_audio_metrics(sysmon_audio_metrics_t *metrics);

/**
 * @brief Record heap snapshot before audio initialization
 *
 * Called by audio_manager_init() to establish baseline.
 *
 * @return ESP_OK on success
 */
esp_err_t sysmon_record_heap_before_audio(void);

/**
 * @brief Record heap snapshot after audio initialization
 *
 * Called by audio_manager_init() to calculate audio memory footprint.
 *
 * @return ESP_OK on success
 */
esp_err_t sysmon_record_heap_after_audio(void);

/**
 * @brief Update recording duration counter
 *
 * Called by recorder module when recording stops.
 *
 * @param duration_ms  Recording duration in milliseconds
 */
void sysmon_update_rec_duration(uint32_t duration_ms);

/**
 * @brief Update playback duration counter
 *
 * Called by player module when playback stops.
 *
 * @param duration_ms  Playback duration in milliseconds
 */
void sysmon_update_play_duration(uint32_t duration_ms);

/**
 * @brief Check thresholds and generate warnings
 *
 * Compares current metrics against configured thresholds.
 * Outputs ESP_LOGW warnings if thresholds exceeded.
 */
void sysmon_check_thresholds(void);

/**
 * @brief Get formatted system status string for display
 *
 * Generates a human-readable status string suitable for
 * rendering on LCD display.
 *
 * @param[out] buf       Output buffer
 * @param[in]  buf_len   Buffer length
 * @return ESP_OK on success
 */
esp_err_t sysmon_get_status_string(char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif