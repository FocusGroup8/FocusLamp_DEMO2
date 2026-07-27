/*
 * motion_analyzer.c - 运动分析实现
 *
 * 根据雷达位置数据检测人体运动状态，通过回调通知状态变更。
 */

#include "motion_analyzer.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "motion_analyzer";

typedef struct {
    radar_motion_state_t current_state;
    int64_t              state_start_time_ms;
    int64_t              sedentary_duration_ms;
    int64_t              stable_start_time_ms;

    motion_position_t current_position;
    motion_position_t position_history[MOTION_POSITION_HISTORY_SIZE];
    size_t            position_history_index;

    float   position_change_cm;
    float   velocity_mps;
    int32_t dop_idx;

    uint32_t active_count;
    uint32_t stable_count;

    motion_history_record_t history[MOTION_HISTORY_MAX_RECORDS];
    size_t                  history_count;
    size_t                  history_index;

    bool enabled;
    bool initialized;
} motion_snapshot_t;

static motion_snapshot_t    s_snap            = {0};
static motion_state_callback_t s_callback     = NULL;
static void                *s_callback_ctx    = NULL;
static bool                 s_human_present   = false;

/* ---- Filter state ---- */
static motion_position_t s_filtered_pos        = {0};
static bool              s_pos_filter_init     = false;
static float             s_filtered_velocity   = 0.0f;
static bool              s_vel_filter_init     = false;

static bool  s_filtered_human_present           = false;
static bool  s_human_present_filter_init         = false;
static uint32_t s_human_present_count           = 0;
static uint32_t s_human_absent_count            = 0;

static float  s_vel_window[MOTION_VELOCITY_AVERAGE_WINDOW_SIZE];
static size_t s_vel_window_idx                  = 0;
static bool   s_vel_window_init                 = false;

static motion_position_t s_pos_window[MOTION_POSITION_AVERAGE_WINDOW_SIZE];
static size_t            s_pos_window_idx       = 0;
static bool              s_pos_window_init      = false;

static void reset_snapshot(void)
{
    memset(&s_snap, 0, sizeof(s_snap));
    s_snap.current_state = RADAR_MOTION_IDLE;
    s_snap.enabled       = true;

    s_pos_filter_init      = false;
    s_vel_filter_init      = false;
    memset(&s_filtered_pos, 0, sizeof(s_filtered_pos));
    s_filtered_velocity    = 0.0f;

    s_human_present_filter_init = false;
    s_filtered_human_present    = false;
    s_human_present_count       = 0;
    s_human_absent_count        = 0;

    s_vel_window_init = false;
    s_vel_window_idx  = 0;
    memset(s_vel_window, 0, sizeof(s_vel_window));

    s_pos_window_init = false;
    s_pos_window_idx  = 0;
    memset(s_pos_window, 0, sizeof(s_pos_window));
}

static void change_state(radar_motion_state_t new_state)
{
    if (s_snap.current_state == new_state) return;

    radar_motion_state_t old_state = s_snap.current_state;
    s_snap.current_state        = new_state;
    s_snap.state_start_time_ms  = esp_timer_get_time() / 1000;
    s_snap.active_count         = 0;
    s_snap.stable_count         = 0;

    ESP_LOGD(TAG, "State: %d -> %d", old_state, new_state);

    if (s_callback != NULL) {
        s_callback(old_state, new_state, s_callback_ctx);
    }
}

static float calc_distance(const motion_position_t *p1, const motion_position_t *p2)
{
    if (p1 == NULL || p2 == NULL) return 0.0f;
    float dx = p2->x - p1->x;
    float dy = p2->y - p1->y;
    float dz = p2->z - p1->z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static float calc_velocity(const motion_position_t *p1, const motion_position_t *p2)
{
    if (p1 == NULL || p2 == NULL) return 0.0f;
    int64_t dt = p2->timestamp_ms - p1->timestamp_ms;
    if (dt <= 0) return 0.0f;
    return calc_distance(p1, p2) / (dt / 1000.0f);
}

static void apply_position_filter(const motion_position_t *raw, motion_position_t *out)
{
    s_pos_window[s_pos_window_idx] = *raw;
    s_pos_window_idx = (s_pos_window_idx + 1) % MOTION_POSITION_AVERAGE_WINDOW_SIZE;

    if (!s_pos_window_init) {
        for (size_t i = 1; i < MOTION_POSITION_AVERAGE_WINDOW_SIZE; i++) {
            s_pos_window[i] = *raw;
        }
        s_pos_window_init = true;
    }

    motion_position_t avg = {0};
    for (size_t i = 0; i < MOTION_POSITION_AVERAGE_WINDOW_SIZE; i++) {
        avg.x += s_pos_window[i].x;
        avg.y += s_pos_window[i].y;
        avg.z += s_pos_window[i].z;
    }
    avg.x /= MOTION_POSITION_AVERAGE_WINDOW_SIZE;
    avg.y /= MOTION_POSITION_AVERAGE_WINDOW_SIZE;
    avg.z /= MOTION_POSITION_AVERAGE_WINDOW_SIZE;
    avg.timestamp_ms = raw->timestamp_ms;

    if (!s_pos_filter_init) {
        *out             = avg;
        s_pos_filter_init = true;
        return;
    }

    out->x            = MOTION_POSITION_FILTER_ALPHA * avg.x +
                        (1.0f - MOTION_POSITION_FILTER_ALPHA) * out->x;
    out->y            = MOTION_POSITION_FILTER_ALPHA * avg.y +
                        (1.0f - MOTION_POSITION_FILTER_ALPHA) * out->y;
    out->z            = MOTION_POSITION_FILTER_ALPHA * avg.z +
                        (1.0f - MOTION_POSITION_FILTER_ALPHA) * out->z;
    out->timestamp_ms = avg.timestamp_ms;
}

static float apply_velocity_filter(float raw_vel)
{
    if (fabsf(raw_vel) < MOTION_VELOCITY_DEAD_ZONE_THRESHOLD) {
        raw_vel = 0.0f;
    }

    if (s_vel_filter_init) {
        if (fabsf(raw_vel - s_filtered_velocity) > MOTION_VELOCITY_JUMP_THRESHOLD) {
            return s_filtered_velocity;
        }
    }

    s_vel_window[s_vel_window_idx] = raw_vel;
    s_vel_window_idx = (s_vel_window_idx + 1) % MOTION_VELOCITY_AVERAGE_WINDOW_SIZE;

    if (!s_vel_window_init) {
        for (size_t i = 1; i < MOTION_VELOCITY_AVERAGE_WINDOW_SIZE; i++) {
            s_vel_window[i] = raw_vel;
        }
        s_vel_window_init = true;
    }

    float sum = 0.0f;
    for (size_t i = 0; i < MOTION_VELOCITY_AVERAGE_WINDOW_SIZE; i++) {
        sum += s_vel_window[i];
    }
    float avg = sum / MOTION_VELOCITY_AVERAGE_WINDOW_SIZE;

    if (!s_vel_filter_init) {
        s_filtered_velocity = avg;
        s_vel_filter_init   = true;
        return s_filtered_velocity;
    }

    s_filtered_velocity = MOTION_VELOCITY_FILTER_ALPHA * avg +
                          (1.0f - MOTION_VELOCITY_FILTER_ALPHA) * s_filtered_velocity;
    return s_filtered_velocity;
}

static void update_state_detection(void)
{
    if (!s_filtered_human_present) {
        change_state(RADAR_MOTION_IDLE);
        s_snap.active_count = 0;
        s_snap.stable_count = 0;
        return;
    }

    if (s_snap.position_history_index < 2) {
        change_state(RADAR_MOTION_ACTIVE);
        return;
    }

    size_t idx            = s_snap.position_history_index;
    const motion_position_t *current  = &s_snap.position_history[idx - 1];
    const motion_position_t *previous = &s_snap.position_history[idx - 2];

    s_snap.position_change_cm = calc_distance(previous, current) * 100.0f;
    float raw_vel = calc_velocity(previous, current);
    s_snap.velocity_mps = apply_velocity_filter(raw_vel);

    bool is_dop_zero        = (s_snap.dop_idx == 0);
    bool is_position_stable = (s_snap.position_change_cm < MOTION_POSITION_STABLE_THRESHOLD_CM);

    if (is_dop_zero && is_position_stable) {
        s_snap.stable_count++;
        s_snap.active_count = 0;

        if (s_snap.stable_count >= MOTION_TOLERANCE_COUNT) {
            if (s_snap.stable_start_time_ms == 0) {
                s_snap.stable_start_time_ms = esp_timer_get_time() / 1000;
            }
            int64_t now = esp_timer_get_time() / 1000;
            s_snap.sedentary_duration_ms = now - s_snap.stable_start_time_ms;

            if (s_snap.sedentary_duration_ms >= MOTION_DURATION_THRESHOLD_MS) {
                change_state(RADAR_MOTION_SEDENTARY);
            } else {
                change_state(RADAR_MOTION_STATIONARY);
            }
        }
    } else if (is_dop_zero) {
        s_snap.stable_count++;
        s_snap.active_count = 0;

        if (s_snap.stable_count >= MOTION_TOLERANCE_COUNT) {
            if (s_snap.current_state != RADAR_MOTION_SEDENTARY &&
                s_snap.current_state != RADAR_MOTION_STATIONARY) {
                change_state(RADAR_MOTION_MICRO_MOTION);
            }
        }
    } else if (!is_position_stable) {
        s_snap.active_count++;
        s_snap.stable_count = 0;
        s_snap.stable_start_time_ms  = 0;
        s_snap.sedentary_duration_ms = 0;

        if (s_snap.active_count >= MOTION_TOLERANCE_COUNT) {
            change_state(RADAR_MOTION_ACTIVE);
        }
    } else {
        s_snap.stable_count++;
        s_snap.active_count = 0;

        if (s_snap.stable_count >= MOTION_TOLERANCE_COUNT) {
            if (s_snap.current_state != RADAR_MOTION_SEDENTARY &&
                s_snap.current_state != RADAR_MOTION_STATIONARY) {
                change_state(RADAR_MOTION_MICRO_MOTION);
            }
        }
    }
}

/* ===================== Public API ===================== */

esp_err_t motion_analyzer_init(void)
{
    if (s_snap.initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    reset_snapshot();
    s_snap.initialized = true;
    s_human_present    = false;

    ESP_LOGI(TAG, "Motion analyzer initialized");
    return ESP_OK;
}

esp_err_t motion_analyzer_deinit(void)
{
    if (!s_snap.initialized) return ESP_OK;

    reset_snapshot();
    s_snap.initialized = false;
    s_human_present    = false;
    s_callback         = NULL;
    s_callback_ctx     = NULL;

    ESP_LOGI(TAG, "Motion analyzer deinitialized");
    return ESP_OK;
}

esp_err_t motion_analyzer_enable(void)
{
    if (!s_snap.initialized) return ESP_ERR_INVALID_STATE;
    s_snap.enabled = true;
    return ESP_OK;
}

esp_err_t motion_analyzer_disable(void)
{
    if (!s_snap.initialized) return ESP_ERR_INVALID_STATE;
    s_snap.enabled = false;
    change_state(RADAR_MOTION_IDLE);
    return ESP_OK;
}

bool motion_analyzer_is_enabled(void)
{
    return s_snap.enabled;
}

radar_motion_state_t motion_analyzer_get_state(void)
{
    return s_snap.current_state;
}

int64_t motion_analyzer_get_sedentary_duration_ms(void)
{
    return s_snap.sedentary_duration_ms;
}

esp_err_t motion_analyzer_update_position(float x, float y, float z,
                                          int32_t dop_idx, int64_t timestamp_ms)
{
    if (!s_snap.initialized || !s_snap.enabled) return ESP_ERR_INVALID_STATE;

    s_snap.current_position.x            = x;
    s_snap.current_position.y            = y;
    s_snap.current_position.z            = z;
    s_snap.current_position.timestamp_ms = timestamp_ms;
    s_snap.dop_idx                       = dop_idx;

    motion_position_t filtered = s_snap.current_position;
    apply_position_filter(&s_snap.current_position, &filtered);

    if (s_snap.position_history_index < MOTION_POSITION_HISTORY_SIZE) {
        s_snap.position_history[s_snap.position_history_index] = filtered;
        s_snap.position_history_index++;
    } else {
        for (size_t i = 1; i < MOTION_POSITION_HISTORY_SIZE; i++) {
            s_snap.position_history[i - 1] = s_snap.position_history[i];
        }
        s_snap.position_history[MOTION_POSITION_HISTORY_SIZE - 1] = filtered;
    }

    update_state_detection();
    return ESP_OK;
}

esp_err_t motion_analyzer_update_human_presence(bool is_present)
{
    if (!s_snap.initialized) return ESP_ERR_INVALID_STATE;

    s_human_present = is_present;

    if (is_present) {
        s_human_present_count++;
        s_human_absent_count = 0;
        if (s_human_present_count >= MOTION_HUMAN_PRESENT_TOLERANCE_COUNT) {
            s_filtered_human_present = true;
        }
    } else {
        s_human_absent_count++;
        s_human_present_count = 0;
        if (s_human_absent_count >= MOTION_HUMAN_PRESENT_TOLERANCE_COUNT) {
            s_filtered_human_present = false;
        }
    }

    if (!s_filtered_human_present) {
        change_state(RADAR_MOTION_IDLE);
    }
    return ESP_OK;
}

esp_err_t motion_analyzer_register_callback(motion_state_callback_t callback, void *user_data)
{
    if (!s_snap.initialized) return ESP_ERR_INVALID_STATE;
    s_callback     = callback;
    s_callback_ctx = user_data;
    return ESP_OK;
}

esp_err_t motion_analyzer_unregister_callback(void)
{
    if (!s_snap.initialized) return ESP_ERR_INVALID_STATE;
    s_callback     = NULL;
    s_callback_ctx = NULL;
    return ESP_OK;
}

bool motion_analyzer_is_initialized(void)
{
    return s_snap.initialized;
}