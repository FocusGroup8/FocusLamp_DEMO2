/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "system_monitor.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

#if CONFIG_EXAMPLE_ENABLE_AUDIO
#include "audio/player.h"
#include "audio/recorder.h"
#endif

static const char *TAG = "SYSMON";

// Thresholds for warnings
#define WARN_THRESHOLD_HEAP_USAGE_PCT 85
#define WARN_THRESHOLD_STACK_HWM_PCT 20

// Module state
static esp_timer_handle_t s_mon_timer = NULL;
static bool s_initialized             = false;
static sysmon_config_t s_config       = {0};

// Audio metrics (only tracked if EXAMPLE_ENABLE_AUDIO)
static sysmon_audio_metrics_t s_audio_metrics = {0};

// Default configuration (uses Kconfig values if available)
static const sysmon_config_t s_default_config = {
#if CONFIG_EXAMPLE_ENABLE_SYSMON
    .interval_sec         = CONFIG_EXAMPLE_SYSMON_INTERVAL_SEC,
    .print_task_list      = CONFIG_EXAMPLE_SYSMON_INTERVAL_SEC,
    .enable_audio_metrics = CONFIG_EXAMPLE_ENABLE_SYSMON,
#else
    .interval_sec         = 10, // Fallback default
    .print_task_list      = false,
    .enable_audio_metrics = false,
#endif
};

//---------------------------------
// Timer callback
//---------------------------------

static void sysmon_timer_cb(void *arg)
{
    sysmon_print_status();
    sysmon_check_thresholds();
}

//---------------------------------
// Public API implementation
//---------------------------------

esp_err_t sysmon_init(const sysmon_config_t *config)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

#if !CONFIG_EXAMPLE_ENABLE_SYSMON
    ESP_LOGW(TAG, "System monitor not enabled in menuconfig (EXAMPLE_ENABLE_SYSMON)");
    ESP_LOGW(TAG, "To enable: idf.py menuconfig → Example Configuration → Enable system resource monitor");
    return ESP_ERR_NOT_SUPPORTED;
#endif

    // Use default config if NULL provided
    if (config == NULL) {
        s_config = s_default_config;
    } else {
        s_config = *config;
    }

    // Create periodic timer
    const esp_timer_create_args_t timer_cfg = {
        .callback = sysmon_timer_cb,
        .name     = "sysmon_timer",
    };
    esp_err_t ret = esp_timer_create(&timer_cfg, &s_mon_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timer: %s", esp_err_to_name(ret));
        return ret;
    }

    // Reset audio metrics
    memset(&s_audio_metrics, 0, sizeof(s_audio_metrics));

    s_initialized = true;
    ESP_LOGI(TAG, "System monitor initialized (interval=%lu sec)", s_config.interval_sec);
    return ESP_OK;
}

void sysmon_deinit(void)
{
    if (!s_initialized) {
        return;
    }

    sysmon_stop();

    if (s_mon_timer) {
        esp_timer_delete(s_mon_timer);
        s_mon_timer = NULL;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "System monitor deinitialized");
}

esp_err_t sysmon_start(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_config.interval_sec == 0) {
        ESP_LOGW(TAG, "Interval is 0, using default 10 sec");
        s_config.interval_sec = 10;
    }

    esp_err_t ret = esp_timer_start_periodic(s_mon_timer, s_config.interval_sec * 1000000ULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start timer: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Periodic monitoring started");
    return ESP_OK;
}

void sysmon_stop(void)
{
    if (s_mon_timer) {
        esp_timer_stop(s_mon_timer);
        ESP_LOGI(TAG, "Periodic monitoring stopped");
    }
}

void sysmon_print_status(void)
{
    sysmon_report_t report;
    sysmon_get_report(&report);

    ESP_LOGI(TAG, "=== System Monitor Report ===");
    ESP_LOGI(TAG, "[Memory]");
    ESP_LOGI(TAG, "  Total Heap:     %zu bytes (%.1f KB)", report.total_heap, report.total_heap / 1024.0f);
    ESP_LOGI(TAG, "  Free Heap:      %zu bytes (%.1f KB) %.1f%% free", report.free_heap, report.free_heap / 1024.0f,
             100.0f - report.heap_usage_pct);
    ESP_LOGI(TAG, "  Min Ever Free:  %zu bytes (%.1f KB)", report.min_free_heap, report.min_free_heap / 1024.0f);
    ESP_LOGI(TAG, "  Largest Block:  %zu bytes (%.1f KB)", report.largest_block, report.largest_block / 1024.0f);

#if CONFIG_SPIRAM
    ESP_LOGI(TAG, "[PSRAM]");
    ESP_LOGI(TAG, "  Total: %zu bytes (%.1f MB)", report.psram_total, report.psram_total / (1024.0f * 1024.0f));
    ESP_LOGI(TAG, "  Free:  %zu bytes (%.1f MB)", report.psram_free, report.psram_free / (1024.0f * 1024.0f));
#endif

    // Task list (optional)
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && CONFIG_EXAMPLE_SYSMON_PRINT_TASK_LIST
    ESP_LOGI(TAG, "[Tasks]");
    char task_buf[512];
    vTaskList(task_buf);
    ESP_LOGI(TAG, "%s", task_buf);
#endif

#if CONFIG_EXAMPLE_ENABLE_AUDIO
    ESP_LOGI(TAG, "[Audio Metrics]");
    ESP_LOGI(TAG, "  Heap used by audio: ~%zu KB",
             s_audio_metrics.heap_after_audio - s_audio_metrics.heap_before_audio);
    ESP_LOGI(TAG, "  Recording duration: %lu ms", s_audio_metrics.rec_duration_ms);
    ESP_LOGI(TAG, "  Playback duration:  %lu ms", s_audio_metrics.play_duration_ms);

    // Audio state
    s_audio_metrics.recorder_state = recorder_get_state();
    s_audio_metrics.player_state   = player_get_state();
    ESP_LOGI(TAG, "  Recorder state: %d", s_audio_metrics.recorder_state);
    ESP_LOGI(TAG, "  Player state:   %d", s_audio_metrics.player_state);
#endif

    ESP_LOGI(TAG, "==============================");
}

esp_err_t sysmon_get_report(sysmon_report_t *report)
{
    if (report == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Heap memory
    report->total_heap     = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    report->free_heap      = esp_get_free_heap_size();
    report->min_free_heap  = esp_get_minimum_free_heap_size();
    report->largest_block  = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    report->heap_usage_pct = (report->total_heap - report->free_heap) * 100.0f / report->total_heap;

#if CONFIG_SPIRAM
    report->psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    report->psram_free  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
#endif

    return ESP_OK;
}

esp_err_t sysmon_get_audio_metrics(sysmon_audio_metrics_t *metrics)
{
#if CONFIG_EXAMPLE_ENABLE_AUDIO
    if (metrics == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Update current states before returning
    s_audio_metrics.recorder_state = recorder_get_state();
    s_audio_metrics.player_state   = player_get_state();

    *metrics = s_audio_metrics;
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t sysmon_record_heap_before_audio(void)
{
#if CONFIG_EXAMPLE_ENABLE_AUDIO
    s_audio_metrics.heap_before_audio = esp_get_free_heap_size();
    ESP_LOGD(TAG, "Heap before audio: %zu bytes", s_audio_metrics.heap_before_audio);
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t sysmon_record_heap_after_audio(void)
{
#if CONFIG_EXAMPLE_ENABLE_AUDIO
    s_audio_metrics.heap_after_audio = esp_get_free_heap_size();
    size_t used                      = s_audio_metrics.heap_before_audio - s_audio_metrics.heap_after_audio;
    ESP_LOGI(TAG, "Audio subsystem memory footprint: ~%zu KB", used / 1024);
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void sysmon_update_rec_duration(uint32_t duration_ms)
{
#if CONFIG_EXAMPLE_ENABLE_AUDIO
    s_audio_metrics.rec_duration_ms += duration_ms;
    ESP_LOGD(TAG, "Recording duration updated: +%lu ms (total: %lu ms)", duration_ms, s_audio_metrics.rec_duration_ms);
#endif
}

void sysmon_update_play_duration(uint32_t duration_ms)
{
#if CONFIG_EXAMPLE_ENABLE_AUDIO
    s_audio_metrics.play_duration_ms += duration_ms;
    ESP_LOGD(TAG, "Playback duration updated: +%lu ms (total: %lu ms)", duration_ms, s_audio_metrics.play_duration_ms);
#endif
}

void sysmon_check_thresholds(void)
{
    sysmon_report_t report;
    sysmon_get_report(&report);

    // Heap usage warning (85% threshold)
    if (report.heap_usage_pct >= WARN_THRESHOLD_HEAP_USAGE_PCT) {
        ESP_LOGW(TAG, "[WARN] Heap usage %.1f%% exceeds threshold %d%%!", report.heap_usage_pct,
                 WARN_THRESHOLD_HEAP_USAGE_PCT);
    }

    // Free heap critically low (32KB threshold)
    if (report.free_heap < 32 * 1024) {
        ESP_LOGW(TAG, "[WARN] Free heap critically low: %zu bytes (< 32KB)", report.free_heap);
    }

    // Free heap warning (64KB threshold)
    if (report.free_heap < 64 * 1024) {
        ESP_LOGW(TAG, "[WARN] Free heap low: %zu bytes (< 64KB)", report.free_heap);
    }

#if CONFIG_SPIRAM
    // PSRAM critically low (256KB threshold)
    if (report.psram_free < 256 * 1024) {
        ESP_LOGW(TAG, "[WARN] PSRAM critically low: %zu bytes (< 256KB)", report.psram_free);
    }

    // PSRAM warning (1MB threshold)
    if (report.psram_free < 1024 * 1024) {
        ESP_LOGW(TAG, "[WARN] PSRAM free low: %zu bytes (< 1MB)", report.psram_free);
    }
#endif

    // Note: Task stack watermarks require task handles which are not stored globally
    // Would need audio manager to expose task handles for monitoring
}

esp_err_t sysmon_get_status_string(char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len < 256) {
        return ESP_ERR_INVALID_ARG;
    }

    sysmon_report_t report;
    sysmon_get_report(&report);

    // Format compact status string for display
    int pos = 0;
    pos += snprintf(buf + pos, buf_len - pos, "=== System Status ===\n");

    pos += snprintf(buf + pos, buf_len - pos, "Heap: %zu/%zu KB (%.1f%%)\n", report.free_heap / 1024,
                    report.total_heap / 1024, 100.0f - report.heap_usage_pct);

    pos += snprintf(buf + pos, buf_len - pos, "Min Free: %zu KB\n", report.min_free_heap / 1024);

#if CONFIG_SPIRAM
    pos += snprintf(buf + pos, buf_len - pos, "PSRAM: %.1f/%.1f MB\n", report.psram_free / (1024.0f * 1024.0f),
                    report.psram_total / (1024.0f * 1024.0f));
#endif

#if CONFIG_EXAMPLE_ENABLE_AUDIO
    pos += snprintf(buf + pos, buf_len - pos, "\n--- Audio ---\n");
    pos += snprintf(buf + pos, buf_len - pos, "Rec: %lu ms\n", s_audio_metrics.rec_duration_ms);
    pos += snprintf(buf + pos, buf_len - pos, "Play: %lu ms\n", s_audio_metrics.play_duration_ms);
#endif

    return ESP_OK;
}

int sysmon_get_task_metrics(sysmon_task_metrics_t *metrics, int max_tasks)
{
    if (metrics == NULL || max_tasks <= 0) {
        return 0;
    }

    int count = 0;

#if CONFIG_EXAMPLE_ENABLE_AUDIO && 0 // Disabled due to API availability issues
    // Simplified: use direct task handle query instead of system state enumeration
    // This avoids dependency on uxTaskGetSystemState() which requires FreeRTOS configuration

    // Would need task handles stored during creation to query stack HWM
    // For now, return 0 to avoid linking issues
#endif

    return count;
}