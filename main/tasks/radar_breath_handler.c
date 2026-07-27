/*
 * radar_breath_handler.c - 呼吸相位处理实现
 *
 * 从雷达相位数据中提取呼吸信号，进行滤波处理。
 */

#include "radar_breath_handler.h"

#include <math.h>

static float    s_last_breath_phase    = 0.0f;
static bool     s_has_breath_phase     = false;
static float    s_filtered_phase       = 0.0f;
static uint32_t s_last_phase_update_ms = 0;

void radar_breath_handler_init(void)
{
    s_last_breath_phase    = 0.0f;
    s_has_breath_phase     = false;
    s_filtered_phase       = 0.0f;
    s_last_phase_update_ms = 0;
}

bool radar_breath_handler_filter_phase(float phase, uint32_t elapsed_ms)
{
    /* Phase range filter */
    if (phase < -0.5f || phase > 0.5f) {
        return false;
    }

    /* Minimum amplitude filter */
    if (fabsf(phase) < 0.02f) {
        return false;
    }

    /* Rate of change filter */
    const float    max_change_rate = 0.2f;
    const uint32_t dt_ms           = elapsed_ms - s_last_phase_update_ms;
    if (dt_ms > 0 && dt_ms < 1000 && s_has_breath_phase) {
        const float max_change = max_change_rate * (dt_ms / 100.0f);
        if (fabsf(phase - s_last_breath_phase) > max_change) {
            return false;
        }
    }

    /* Duplicate filter */
    if (s_has_breath_phase && fabsf(phase - s_last_breath_phase) < 0.001f) {
        return false;
    }

    return true;
}

static float radar_breath_handler_apply_lowpass_filter(float new_value, float old_value, float alpha)
{
    return old_value + alpha * (new_value - old_value);
}

float radar_breath_handler_get_filtered_phase(void)
{
    return s_filtered_phase;
}

bool radar_breath_handler_has_phase(void)
{
    return s_has_breath_phase;
}

void radar_breath_handler_update(float phase, uint32_t elapsed_ms)
{
    if (radar_breath_handler_filter_phase(phase, elapsed_ms)) {
        s_filtered_phase = radar_breath_handler_apply_lowpass_filter(phase, s_filtered_phase, 0.3f);
        s_last_breath_phase    = s_filtered_phase;
        s_has_breath_phase     = true;
        s_last_phase_update_ms = elapsed_ms;
    }
}