/*
 * hrv_analyzer.h - HRV 分析接口
 *
 * 心率变异性分析模块，从心跳相位数据检测峰值，
 * 计算 RR 间隔，并统计 SDNN/RMSSD/pNN50 等指标。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== Configuration ===================== */
#define HRV_MAX_PEAK_HISTORY        100
#define HRV_MAX_RR_INTERVALS        50
#define HRV_PHASE_THRESHOLD         0.1f
#define HRV_MIN_PEAK_INTERVAL_MS    300
#define HRV_MAX_PEAK_INTERVAL_MS    2000
#define HRV_FILTER_ALPHA            0.3f
#define HRV_MIN_PEAK_AMPLITUDE      0.01f
#define HRV_RR_OUTLIER_THRESHOLD    2.0f
#define HRV_MIN_HEART_RATE_BPM      40.0f
#define HRV_MAX_HEART_RATE_BPM      200.0f

/* ===================== Status ===================== */
typedef enum {
    HRV_ANALYZER_STATUS_IDLE = 0,
    HRV_ANALYZER_STATUS_RUNNING,
    HRV_ANALYZER_STATUS_ERROR,
} hrv_analyzer_status_t;

/* ===================== Data Structures ===================== */
typedef struct {
    float timestamp_ms;
    float phase_value;
} hrv_peak_t;

typedef struct {
    float rr_interval_ms;
    float timestamp_ms;
} hrv_rr_interval_t;

typedef struct {
    float  sdnn;
    float  rmssd;
    float  mean_rr;
    float  mean_hr;
    float  pnn50;
    size_t valid_rr_count;
    size_t total_peaks;
    float  analysis_window_seconds;
} hrv_metrics_t;

/* ===================== Public API ===================== */

esp_err_t hrv_analyzer_init(void);
esp_err_t hrv_analyzer_deinit(void);

esp_err_t hrv_analyzer_feed_phase(float heart_phase, int64_t timestamp_ms);

esp_err_t hrv_analyzer_get_metrics(hrv_metrics_t *metrics);
esp_err_t hrv_analyzer_reset(void);

bool hrv_analyzer_is_initialized(void);
bool hrv_analyzer_has_new_metrics(void);
esp_err_t hrv_analyzer_clear_new_metrics_flag(void);

#ifdef __cplusplus
}
#endif