/*
 * hrv_analyzer.c - HRV 分析实现
 *
 * 从心跳相位数据检测峰值，计算 RR 间隔，
 * 使用生理范围 + MAD 两级过滤，计算 SDNN/RMSSD/pNN50。
 */

#include "hrv_analyzer.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "hrv_analyzer";

#define HRV_DERIVATIVE_THRESHOLD      0.005f
#define HRV_PEAK_CONFIRM_SAMPLES      3
#define HRV_MAD_SCALE_FACTOR          1.4826f
#define HRV_PHASE_HISTORY_SIZE        5

static hrv_analyzer_status_t s_status         = HRV_ANALYZER_STATUS_IDLE;
static bool                  s_initialized    = false;
static bool                  s_has_new_metrics = false;
static hrv_metrics_t         s_last_metrics   = {0};

/* Peak detection state */
static hrv_peak_t        s_peak_history[HRV_MAX_PEAK_HISTORY];
static size_t            s_peak_count             = 0;
static hrv_rr_interval_t s_rr_intervals[HRV_MAX_RR_INTERVALS];
static size_t            s_rr_count               = 0;

static float    s_filtered_phase       = 0.0f;
static float    s_last_phase           = 0.0f;
static float    s_last_derivative      = 0.0f;
static float    s_last_valley          = 0.0f;
static int64_t  s_last_peak_time_ms    = 0;
static float    s_phase_threshold      = HRV_PHASE_THRESHOLD;
static bool     s_is_rising            = false;
static int64_t  s_start_time_ms        = 0;

/* Pending peak confirmation */
static float   s_pending_peak_value     = 0.0f;
static int64_t s_pending_peak_time_ms   = 0;
static float   s_pending_peak_amplitude = 0.0f;
static uint32_t s_decline_count         = 0;
static bool    s_has_pending_peak       = false;

/* Median filter history */
static float  s_phase_history[HRV_PHASE_HISTORY_SIZE];
static size_t s_phase_history_index = 0;

static void reset_internal(void)
{
    s_filtered_phase       = 0.0f;
    s_last_phase           = 0.0f;
    s_last_derivative      = 0.0f;
    s_last_valley          = 0.0f;
    s_last_peak_time_ms    = 0;
    s_phase_threshold      = HRV_PHASE_THRESHOLD;
    s_is_rising            = false;
    s_pending_peak_value   = 0.0f;
    s_pending_peak_time_ms = 0;
    s_pending_peak_amplitude = 0.0f;
    s_decline_count        = 0;
    s_has_pending_peak     = false;

    memset(s_phase_history, 0, sizeof(s_phase_history));
    s_phase_history_index = 0;
}

static void reset_snapshot(void)
{
    memset(s_peak_history, 0, sizeof(s_peak_history));
    s_peak_count = 0;
    memset(s_rr_intervals, 0, sizeof(s_rr_intervals));
    s_rr_count           = 0;
    s_has_new_metrics    = false;
    memset(&s_last_metrics, 0, sizeof(s_last_metrics));
    s_start_time_ms      = 0;
    reset_internal();
}

/* ---- Filters ---- */
static float lowpass_filter(float new_value, float old_value, float alpha)
{
    return old_value + alpha * (new_value - old_value);
}

static float median_filter(float new_value)
{
    s_phase_history[s_phase_history_index] = new_value;
    s_phase_history_index = (s_phase_history_index + 1) % HRV_PHASE_HISTORY_SIZE;

    float sorted[HRV_PHASE_HISTORY_SIZE];
    memcpy(sorted, s_phase_history, sizeof(s_phase_history));

    for (size_t i = 0; i < HRV_PHASE_HISTORY_SIZE - 1; i++) {
        for (size_t j = i + 1; j < HRV_PHASE_HISTORY_SIZE; j++) {
            if (sorted[i] > sorted[j]) {
                float t    = sorted[i];
                sorted[i]  = sorted[j];
                sorted[j]  = t;
            }
        }
    }
    return sorted[2];
}

/* ---- Statistics ---- */
static float calc_mean(const float *data, size_t count)
{
    if (count == 0) return 0.0f;
    float sum = 0.0f;
    for (size_t i = 0; i < count; i++) sum += data[i];
    return sum / (float)count;
}

static float calc_std(const float *data, size_t count, float mean)
{
    if (count == 0) return 0.0f;
    float sum = 0.0f;
    for (size_t i = 0; i < count; i++) {
        float d = data[i] - mean;
        sum += d * d;
    }
    return sqrtf(sum / (float)count);
}

static float calc_rmssd(const float *data, size_t count)
{
    if (count < 2) return 0.0f;
    float sum = 0.0f;
    size_t n  = 0;
    for (size_t i = 1; i < count; i++) {
        float d = data[i] - data[i - 1];
        sum += d * d;
        n++;
    }
    return (n == 0) ? 0.0f : sqrtf(sum / (float)n);
}

static float calc_pnn50(const float *data, size_t count)
{
    if (count < 2) return 0.0f;
    size_t nn50 = 0;
    size_t n    = 0;
    for (size_t i = 1; i < count; i++) {
        if (fabsf(data[i] - data[i - 1]) > 50.0f) nn50++;
        n++;
    }
    return (n == 0) ? 0.0f : (float)nn50 / (float)n * 100.0f;
}

static int cmp_float(const void *a, const void *b)
{
    float fa = *(const float *)a;
    float fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

static float calc_median(float *data, size_t count)
{
    if (count == 0) return 0.0f;
    qsort(data, count, sizeof(float), cmp_float);
    if (count % 2 == 0) {
        return (data[count / 2 - 1] + data[count / 2]) / 2.0f;
    }
    return data[count / 2];
}

static float calc_mad(const float *data, size_t count, float median)
{
    if (count == 0) return 0.0f;
    float dev[HRV_MAX_RR_INTERVALS];
    size_t n = (count < HRV_MAX_RR_INTERVALS) ? count : HRV_MAX_RR_INTERVALS;
    for (size_t i = 0; i < n; i++) {
        dev[i] = fabsf(data[i] - median);
    }
    return calc_median(dev, n);
}

static void update_metrics(void)
{
    if (s_rr_count < 2) return;

    float rr_vals[HRV_MAX_RR_INTERVALS];
    for (size_t i = 0; i < s_rr_count && i < HRV_MAX_RR_INTERVALS; i++) {
        rr_vals[i] = s_rr_intervals[i].rr_interval_ms;
    }
    size_t rr_cnt = (s_rr_count < HRV_MAX_RR_INTERVALS) ? s_rr_count : HRV_MAX_RR_INTERVALS;

    /* Stage 1: 生理范围过滤 */
    float physio[HRV_MAX_RR_INTERVALS];
    size_t pc = 0;
    for (size_t i = 0; i < rr_cnt; i++) {
        float hr = 60000.0f / rr_vals[i];
        if (hr >= HRV_MIN_HEART_RATE_BPM && hr <= HRV_MAX_HEART_RATE_BPM) {
            physio[pc++] = rr_vals[i];
        }
    }
    if (pc < 2) return;

    /* Stage 2: MAD 异常值过滤 */
    float median_rr = calc_median(physio, pc);
    float mad       = calc_mad(physio, pc, median_rr);
    float robust_std = HRV_MAD_SCALE_FACTOR * mad;

    float filtered[HRV_MAX_RR_INTERVALS];
    size_t fc = 0;

    if (robust_std > 0.001f) {
        for (size_t i = 0; i < pc; i++) {
            float z = HRV_MAD_SCALE_FACTOR * (physio[i] - median_rr) / mad;
            if (fabsf(z) < HRV_RR_OUTLIER_THRESHOLD) {
                filtered[fc++] = physio[i];
            }
        }
    } else {
        memcpy(filtered, physio, pc * sizeof(float));
        fc = pc;
    }
    if (fc < 2) return;

    float mean_rr = calc_mean(filtered, fc);
    float sdnn    = calc_std(filtered, fc, mean_rr);
    float rmssd   = calc_rmssd(filtered, fc);
    float pnn50   = calc_pnn50(filtered, fc);
    float mean_hr = (mean_rr > 0.0f) ? 60000.0f / mean_rr : 0.0f;

    s_last_metrics.sdnn           = sdnn;
    s_last_metrics.rmssd          = rmssd;
    s_last_metrics.mean_rr        = mean_rr;
    s_last_metrics.mean_hr        = mean_hr;
    s_last_metrics.pnn50          = pnn50;
    s_last_metrics.valid_rr_count = fc;
    s_last_metrics.total_peaks    = s_peak_count;

    if (s_start_time_ms > 0) {
        int64_t now = esp_timer_get_time() / 1000;
        s_last_metrics.analysis_window_seconds = (now - s_start_time_ms) / 1000.0f;
    }

    s_has_new_metrics = true;

    ESP_LOGD(TAG, "HRV: SDNN=%.2f ms, RMSSD=%.2f ms, MeanHR=%.1f bpm, pNN50=%.1f%%, ValidRR=%u/%u",
             sdnn, rmssd, mean_hr, pnn50, (unsigned)fc, (unsigned)rr_cnt);
}

static void add_peak(int64_t timestamp_ms, float phase_value, float amplitude)
{
    if (s_peak_count >= HRV_MAX_PEAK_HISTORY) {
        memmove(s_peak_history, s_peak_history + 1,
                (HRV_MAX_PEAK_HISTORY - 1) * sizeof(hrv_peak_t));
        s_peak_count--;
    }
    s_peak_history[s_peak_count].timestamp_ms = (float)timestamp_ms;
    s_peak_history[s_peak_count].phase_value  = phase_value;
    s_peak_count++;

    if (s_last_peak_time_ms > 0) {
        float rr = (float)(timestamp_ms - s_last_peak_time_ms);
        if (rr >= HRV_MIN_PEAK_INTERVAL_MS && rr <= HRV_MAX_PEAK_INTERVAL_MS) {
            if (s_rr_count >= HRV_MAX_RR_INTERVALS) {
                memmove(s_rr_intervals, s_rr_intervals + 1,
                        (HRV_MAX_RR_INTERVALS - 1) * sizeof(hrv_rr_interval_t));
                s_rr_count--;
            }
            s_rr_intervals[s_rr_count].rr_interval_ms = rr;
            s_rr_intervals[s_rr_count].timestamp_ms   = (float)timestamp_ms;
            s_rr_count++;

            if (s_rr_count >= 5) {
                update_metrics();
            }
        }
    }
    s_last_peak_time_ms = timestamp_ms;
}

/* ===================== Public API ===================== */

esp_err_t hrv_analyzer_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }
    reset_snapshot();
    s_status       = HRV_ANALYZER_STATUS_RUNNING;
    s_start_time_ms = esp_timer_get_time() / 1000;
    s_initialized  = true;
    ESP_LOGI(TAG, "HRV analyzer initialized");
    return ESP_OK;
}

esp_err_t hrv_analyzer_deinit(void)
{
    if (!s_initialized) return ESP_OK;
    reset_snapshot();
    s_status      = HRV_ANALYZER_STATUS_IDLE;
    s_initialized = false;
    ESP_LOGI(TAG, "HRV analyzer deinitialized");
    return ESP_OK;
}

esp_err_t hrv_analyzer_feed_phase(float heart_phase, int64_t timestamp_ms)
{
    if (!s_initialized || s_status != HRV_ANALYZER_STATUS_RUNNING) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 中值 + 低通滤波 */
    float median = median_filter(heart_phase);
    s_filtered_phase = lowpass_filter(median, s_filtered_phase, HRV_FILTER_ALPHA);

    float derivative = s_filtered_phase - s_last_phase;

    if (s_is_rising) {
        if (derivative < -HRV_DERIVATIVE_THRESHOLD) {
            if (!s_has_pending_peak) {
                s_pending_peak_value     = s_last_phase;
                s_pending_peak_time_ms   = timestamp_ms;
                s_pending_peak_amplitude = s_last_phase - s_last_valley;
                s_has_pending_peak       = true;
                s_decline_count          = 1;
            } else {
                s_decline_count++;
            }

            if (s_has_pending_peak && s_decline_count >= HRV_PEAK_CONFIRM_SAMPLES) {
                if (s_pending_peak_value > s_phase_threshold &&
                    s_pending_peak_amplitude > HRV_MIN_PEAK_AMPLITUDE) {
                    bool accept = true;
                    if (s_last_peak_time_ms > 0) {
                        float dt = (float)(s_pending_peak_time_ms - s_last_peak_time_ms);
                        if (dt < HRV_MIN_PEAK_INTERVAL_MS) accept = false;
                    }
                    if (accept) {
                        add_peak(s_pending_peak_time_ms, s_pending_peak_value,
                                 s_pending_peak_amplitude);
                    }
                }
                s_has_pending_peak = false;
                s_decline_count    = 0;
                s_is_rising        = false;
            }
        } else if (derivative > HRV_DERIVATIVE_THRESHOLD) {
            s_has_pending_peak = false;
            s_decline_count    = 0;
        }
    } else {
        if (derivative > HRV_DERIVATIVE_THRESHOLD) {
            s_last_valley    = s_filtered_phase;
            s_has_pending_peak = false;
            s_decline_count    = 0;
            s_is_rising        = true;
        }
    }

    s_last_phase      = s_filtered_phase;
    s_last_derivative = derivative;
    return ESP_OK;
}

esp_err_t hrv_analyzer_get_metrics(hrv_metrics_t *metrics)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (metrics == NULL) return ESP_ERR_INVALID_ARG;
    *metrics = s_last_metrics;
    return ESP_OK;
}

esp_err_t hrv_analyzer_reset(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    reset_snapshot();
    s_status        = HRV_ANALYZER_STATUS_RUNNING;
    s_start_time_ms = esp_timer_get_time() / 1000;
    return ESP_OK;
}

bool hrv_analyzer_is_initialized(void)
{
    return s_initialized;
}

bool hrv_analyzer_has_new_metrics(void)
{
    return s_has_new_metrics;
}

esp_err_t hrv_analyzer_clear_new_metrics_flag(void)
{
    s_has_new_metrics = false;
    return ESP_OK;
}