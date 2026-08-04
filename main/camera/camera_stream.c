/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "camera_stream.h"

#include "driver/isp_hist.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "websocket_manager.h"

#include <string.h>

static const char *TAG = "cam_stream";

#define STREAM_TASK_STACK_DEFAULT 8192
#define STREAM_TASK_PRIORITY_DEFAULT 5
#define STATS_WINDOW_FRAMES 30

/* Minimum FPS when no camera client is connected (throttle to avoid flooding) */
#define ADAPTIVE_FPS_MIN 2

/*---------------------------------------------------------------
 * BBA (Buffer-Based Algorithm) Adaptive Streaming
 *-------------------------------------------------------------*/
/* Quality levels: each defines JPEG quality and target FPS.
 * Constraint: JPEG quality must be ≥ 15 (user requirement).
 * Based on empirical estimates at 800x640 YUV420:
 * LOW:  ~10KB/frame @ 5fps  → ~50KB/s  (0.40 Mbps) - very stable
 * MID:  ~10KB/frame @ 10fps → ~100KB/s (0.80 Mbps) - stable, current baseline
 * HIGH: ~13KB/frame @ 12fps → ~156KB/s (1.25 Mbps) - best effort */
typedef enum { BBA_LEVEL_LOW = 0, BBA_LEVEL_MID = 1, BBA_LEVEL_HIGH = 2, BBA_LEVEL_COUNT } bba_level_t;

typedef struct {
    int quality;
    int fps;
} bba_level_config_t;

static const bba_level_config_t s_bba_levels[BBA_LEVEL_COUNT] = {
    [BBA_LEVEL_LOW]  = {.quality = 15, .fps = 5},
    [BBA_LEVEL_MID]  = {.quality = 16, .fps = 10},
    [BBA_LEVEL_HIGH] = {.quality = 20, .fps = 12},
};

/* BBA evaluation interval (seconds) */
#define BBA_EVAL_INTERVAL_SEC 1

/* Hysteresis: consecutive samples required before level switch */
#define BBA_HYSTERESIS_THRESHOLD 2

/* Minimum send attempts before allowing step-up (prevents false healthy
 * assessment when client just connected and few frames sent) */
#define BBA_MIN_SAMPLES_FOR_STEPUP 5

/* Client stable connection time (seconds): client must be connected for at
 * least this duration before BBA allows step-up. Prevents oscillation caused
 * by rapid connect/disconnect cycles where WS stats reset to 0 failures. */
#define BBA_CLIENT_STABLE_TIME_SEC 10

/* Congestion thresholds */
#define BBA_FAILURE_RATE_HIGH 0.20f /* >20% failure rate → step down */
#define BBA_FAILURE_RATE_LOW 0.05f  /* <5% failure rate → can step up */
#define BBA_CONSEC_FAIL_HIGH 3      /* >3 consecutive failures → immediate step down */
#define BBA_SEND_DUR_HIGH_MS 200    /* >200ms avg send duration → congested */
#define BBA_SEND_DUR_LOW_MS 50      /* <50ms avg send duration → healthy */

/* BBA state */
typedef struct {
    bba_level_t current_level;
    bba_level_t pending_level;
    int hysteresis_counter;
    int64_t last_eval_time;
    /* Accumulated stats for 10-sec log (since BBA resets WS stats every 1 sec) */
    uint32_t accum_sent;
    uint32_t accum_failed;
    /* Client stable connection tracking (anti-oscillation) */
    int prev_client_count;
    int64_t client_connect_time; /* Timestamp when client connected */
} bba_state_t;

/* Evaluate BBA level based on WebSocket send statistics.
 * Returns the current (possibly updated) BBA level. */
static bba_level_t bba_evaluate(bba_state_t *bba, const ws_send_stats_t *stats, int client_count)
{
    int64_t now = esp_timer_get_time();

    /* Track client connection state transitions */
    if (client_count > 0 && bba->prev_client_count == 0) {
        /* Client just connected (from 0): record timestamp */
        bba->client_connect_time = now;
        bba->hysteresis_counter  = 0;
        bba->pending_level       = bba->current_level;
        ESP_LOGI(TAG, "BBA: client connected, stable period %d sec started", BBA_CLIENT_STABLE_TIME_SEC);
    } else if (client_count < bba->prev_client_count) {
        /* Client disconnected (any client): reset stable time.
         * This handles multi-client scenarios where client_count never
         * reaches 0 but a disconnect-reconnect cycle still occurred. */
        bba->client_connect_time = now;
        bba->hysteresis_counter  = 0;
        bba->pending_level       = bba->current_level;
        ESP_LOGI(TAG, "BBA: client disconnected (%d->%d), stable period reset", bba->prev_client_count, client_count);
    }
    bba->prev_client_count = client_count;

    /* No clients: freeze at current level, reset hysteresis */
    if (client_count == 0) {
        bba->hysteresis_counter = 0;
        bba->pending_level      = bba->current_level;
        return bba->current_level;
    }

    /* Compute client stable duration */
    int64_t client_stable_sec = (now - bba->client_connect_time) / 1000000;

    /* Compute failure rate over the sampling window */
    uint32_t total          = stats->total_sent + stats->total_failed;
    float failure_rate      = (total > 0) ? (float)stats->total_failed / (float)total : 0.0f;
    int64_t avg_duration_ms = stats->avg_send_duration_us / 1000;

    /* Determine target level based on network conditions */
    bba_level_t target = bba->current_level;

    /* Step down: severe congestion (immediate) or sustained congestion */
    if (stats->consecutive_failures >= BBA_CONSEC_FAIL_HIGH || failure_rate > BBA_FAILURE_RATE_HIGH ||
        avg_duration_ms > BBA_SEND_DUR_HIGH_MS) {
        if (bba->current_level > BBA_LEVEL_LOW) {
            target = (bba_level_t)(bba->current_level - 1);
        }
    }
    /* Step up: network healthy — require ALL conditions:
     * 1. Minimum sample count (prevent false healthy on few frames)
     * 2. Client stable connection time (prevent oscillation on reconnect)
     * 3. Low failure rate and fast send duration */
    else if (total >= BBA_MIN_SAMPLES_FOR_STEPUP && client_stable_sec >= BBA_CLIENT_STABLE_TIME_SEC &&
             failure_rate < BBA_FAILURE_RATE_LOW && avg_duration_ms < BBA_SEND_DUR_LOW_MS &&
             bba->current_level < BBA_LEVEL_HIGH) {
        target = (bba_level_t)(bba->current_level + 1);
    }

    /* Apply hysteresis: require N consecutive samples to confirm switch */
    if (target != bba->current_level) {
        if (target == bba->pending_level) {
            bba->hysteresis_counter++;
            if (bba->hysteresis_counter >= BBA_HYSTERESIS_THRESHOLD) {
                bba->current_level      = target;
                bba->hysteresis_counter = 0;
                bba->pending_level      = target;
                ESP_LOGI(TAG,
                         "BBA: level switched to %d (Q=%d, fps=%d) "
                         "[fail_rate=%.1f%% consec=%u dur=%lldms]",
                         target, s_bba_levels[target].quality, s_bba_levels[target].fps, failure_rate * 100.0f,
                         stats->consecutive_failures, (long long)avg_duration_ms);
            }
        } else {
            bba->pending_level      = target;
            bba->hysteresis_counter = 1;
        }
    } else {
        /* Network stable at current level: reset hysteresis */
        bba->hysteresis_counter = 0;
        bba->pending_level      = bba->current_level;
    }

    return bba->current_level;
}

/*---------------------------------------------------------------
 * PI Controller for JPEG Quality Fine-Tuning
 *
 * Architecture: BBA (coarse) + PI (fine) layered control.
 * BBA selects discrete quality level (LOW/MID/HIGH) based on
 * network congestion signals. PI controller fine-tunes JPEG quality
 * within the current BBA level to track the target bitrate.
 *-------------------------------------------------------------*/

/* Target bitrate per BBA level (bytes/sec) */
static const uint32_t s_bba_target_bitrate[BBA_LEVEL_COUNT] = {
    [BBA_LEVEL_LOW]  = 50 * 1024,  /*  50 KB/s (0.40 Mbps) */
    [BBA_LEVEL_MID]  = 100 * 1024, /* 100 KB/s (0.80 Mbps) */
    [BBA_LEVEL_HIGH] = 156 * 1024, /* 156 KB/s (1.25 Mbps) */
};

/* Q value range constraints per BBA level.
 * Ranges overlap to ensure continuous Q transition across level switches. */
typedef struct {
    int q_min;
    int q_max;
} bba_q_range_t;

static const bba_q_range_t s_bba_q_ranges[BBA_LEVEL_COUNT] = {
    [BBA_LEVEL_LOW]  = {.q_min = 15, .q_max = 18},
    [BBA_LEVEL_MID]  = {.q_min = 15, .q_max = 22},
    [BBA_LEVEL_HIGH] = {.q_min = 18, .q_max = 35},
};

/* PI controller parameters */
#define PI_KP 0.5f /* Proportional gain */
#define PI_KI 0.1f /* Integral gain */
#define PI_Q_SCALE                                                            \
    20.0f                        /* Output amplification: converts normalized \
                                  * error to Q adjustment (15% error → 1-3 Q steps) */
#define PI_INTEGRAL_LIMIT 15.0f  /* Anti-windup: integral clamp */
#define PI_EMA_ALPHA 0.15f       /* Output EMA smoothing coefficient */
#define PI_DEADZONE_RATIO 0.05f  /* ±5% dead zone */
#define PI_MIN_INTERVAL_FRAMES 3 /* Min frames between Q changes */

/* PI controller state */
typedef struct {
    float integral;
    int smoothed_quality;
    uint32_t target_bitrate_bps;
    uint32_t actual_bitrate_bps;
    bool initialized;
} pi_state_t;

/* Reset PI controller state (called on BBA level change) */
static void pi_controller_reset(pi_state_t *pi, int base_quality, uint32_t target_bitrate)
{
    pi->integral           = 0.0f;
    pi->smoothed_quality   = base_quality;
    pi->actual_bitrate_bps = 0;
    pi->target_bitrate_bps = target_bitrate;
    pi->initialized        = true;
}

/* Update PI controller and return new JPEG quality.
 * Called every 1 second (aligned with BBA evaluation).
 *
 * actual_bitrate_bps: measured bytes sent in the last 1-sec window
 * target_bitrate_bps: target bytes/sec for current BBA level
 * base_quality: BBA-defined base quality for current level
 * q_range: min/max Q bounds for current BBA level
 *
 * Control logic:
 * - Error = normalized difference between actual and target bitrate
 * - Dead zone (±5%): small errors ignored to prevent oscillation
 * - Anti-windup: integral clamped to ±15
 * - EMA smoothing: output filtered with α=0.15
 *
 * Note: PI_MIN_INTERVAL_FRAMES (3) is inherently satisfied because PI
 * runs at 1Hz while FPS ≥ 5, meaning ≥5 frames pass between updates.
 */
static int pi_controller_update(pi_state_t *pi, uint32_t actual_bitrate_bps, uint32_t target_bitrate_bps,
                                int base_quality, const bba_q_range_t *q_range)
{
    pi->actual_bitrate_bps = actual_bitrate_bps;
    pi->target_bitrate_bps = target_bitrate_bps;

    /* Compute normalized error with dead zone */
    float error = 0.0f;
    if (target_bitrate_bps > 0) {
        float ratio = (float)actual_bitrate_bps / (float)target_bitrate_bps;
        if (ratio > (1.0f + PI_DEADZONE_RATIO)) {
            /* Bitrate too high: negative error → reduce Q */
            error = -((float)(actual_bitrate_bps - target_bitrate_bps) / (float)target_bitrate_bps);
        } else if (ratio < (1.0f - PI_DEADZONE_RATIO)) {
            /* Bitrate too low: positive error → increase Q */
            error = (float)(target_bitrate_bps - actual_bitrate_bps) / (float)target_bitrate_bps;
        }
        /* else: within dead zone, error = 0 */
    }

    /* Update integral with anti-windup */
    pi->integral += error * PI_KI;
    if (pi->integral > PI_INTEGRAL_LIMIT) {
        pi->integral = PI_INTEGRAL_LIMIT;
    } else if (pi->integral < -PI_INTEGRAL_LIMIT) {
        pi->integral = -PI_INTEGRAL_LIMIT;
    }

    /* PI output: quality adjustment (amplified by Q_SCALE for visible Q steps).
     * With PI_KP=0.5, PI_Q_SCALE=20: 15% error → 1.5 Q steps,
     * 30% error → 3 Q steps. Integral accumulates for steady-state correction. */
    float adjustment = (PI_KP * error + pi->integral) * PI_Q_SCALE;
    int target_q     = base_quality + (int)adjustment;

    /* Clamp to BBA level Q range */
    if (target_q < q_range->q_min)
        target_q = q_range->q_min;
    if (target_q > q_range->q_max)
        target_q = q_range->q_max;

    /* EMA smoothing */
    pi->smoothed_quality = (int)(PI_EMA_ALPHA * target_q + (1.0f - PI_EMA_ALPHA) * pi->smoothed_quality);

    return pi->smoothed_quality;
}

/*---------------------------------------------------------------
 * Motion Detection via ISP HIST
 *
 * Uses ISP hardware histogram (16-bin Y luminance) to detect scene
 * changes. When scene is static for N consecutive frames, reduces
 * FPS to save bandwidth. Motion is detected immediately, restoring
 * full FPS.
 *
 * Algorithm: Normalized SAD (Sum of Absolute Differences) between
 * consecutive frame histograms. Hysteresis prevents oscillation.
 *-------------------------------------------------------------*/

#define MOTION_HIST_BINS 16           /* ISP_HIST_SEGMENT_NUMS (ESP32-P4) */
#define MOTION_STATIC_FPS 3           /* FPS when scene is static */
#define MOTION_STATIC_FRAMES 8        /* Consecutive static frames to trigger */
#define MOTION_STATIC_THRESHOLD 0.03f /* Normalized SAD < 3% → static */
#define MOTION_ACTIVE_THRESHOLD 0.08f /* Normalized SAD > 8% → motion */

/* HIST ISR-to-task communication (protected by spinlock) */
static portMUX_TYPE s_hist_spinlock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_latest_hist[MOTION_HIST_BINS];
static volatile bool s_hist_ready = false;

/* HIST ISR callback: copies histogram to static buffer.
 * Runs in ISR context — must be fast and IRAM-safe. */
static bool IRAM_ATTR s_hist_stats_done_cb(isp_hist_ctlr_t hist_ctlr, const esp_isp_hist_evt_data_t *edata,
                                           void *user_data)
{
    (void)hist_ctlr;
    (void)user_data;
    portENTER_CRITICAL_ISR(&s_hist_spinlock);
    memcpy(s_latest_hist, edata->hist_result.hist_value, sizeof(uint32_t) * MOTION_HIST_BINS);
    s_hist_ready = true;
    portEXIT_CRITICAL_ISR(&s_hist_spinlock);
    return false;
}

/* Motion detection state */
typedef struct {
    uint32_t prev_hist[MOTION_HIST_BINS];
    bool has_prev;
    int static_count;
    bool is_static;
    float last_sad;
} motion_state_t;

/* Compute normalized SAD between current and previous histogram.
 * Returns value 0.0 (identical) to 1.0 (completely different). */
static float motion_compute_sad(const uint32_t *curr, const uint32_t *prev)
{
    uint32_t sad   = 0;
    uint32_t total = 0;
    for (int i = 0; i < MOTION_HIST_BINS; i++) {
        sad += (curr[i] > prev[i]) ? (curr[i] - prev[i]) : (prev[i] - curr[i]);
        total += curr[i];
    }
    if (total == 0)
        return 0.0f;
    return (float)sad / (float)total;
}

/* Update motion state with latest histogram.
 * Returns true if scene is static (should reduce FPS). */
static bool motion_update(motion_state_t *ms, const uint32_t *curr_hist)
{
    if (!ms->has_prev) {
        /* First frame: no comparison possible, assume active */
        memcpy(ms->prev_hist, curr_hist, sizeof(uint32_t) * MOTION_HIST_BINS);
        ms->has_prev     = true;
        ms->is_static    = false;
        ms->static_count = 0;
        ms->last_sad     = 0.0f;
        return false;
    }

    ms->last_sad = motion_compute_sad(curr_hist, ms->prev_hist);

    /* Hysteresis: different thresholds for static→active vs active→static */
    if (ms->is_static) {
        /* Currently static: exit only on significant motion */
        if (ms->last_sad > MOTION_ACTIVE_THRESHOLD) {
            ms->is_static    = false;
            ms->static_count = 0;
        }
    } else {
        /* Currently active: count consecutive static frames */
        if (ms->last_sad < MOTION_STATIC_THRESHOLD) {
            ms->static_count++;
            if (ms->static_count >= MOTION_STATIC_FRAMES) {
                ms->is_static = true;
            }
        } else {
            ms->static_count = 0;
        }
    }

    memcpy(ms->prev_hist, curr_hist, sizeof(uint32_t) * MOTION_HIST_BINS);
    return ms->is_static;
}

typedef struct {
    camera_stream_config_t config;
    bool initialized;
    bool running;
    TaskHandle_t task_handle;
    SemaphoreHandle_t lock;

    /* Runtime state */
    int current_quality;
    int current_fps;

    /* Statistics */
    uint32_t frames_sent;
    uint32_t frames_failed;
    uint32_t frame_size_sum;
    uint32_t frame_size_count;
    int64_t last_stats_time;
    float actual_fps;
} stream_state_t;

static stream_state_t s_state = {0};

/*---------------------------------------------------------------
 * Internal: Streaming task
 *-------------------------------------------------------------*/
static void camera_stream_task(void *arg)
{
    /* Initialize BBA state — start at MID level (current baseline) */
    bba_state_t bba = {
        .current_level       = BBA_LEVEL_MID,
        .pending_level       = BBA_LEVEL_MID,
        .hysteresis_counter  = 0,
        .last_eval_time      = esp_timer_get_time(),
        .accum_sent          = 0,
        .accum_failed        = 0,
        .prev_client_count   = 0,
        .client_connect_time = 0,
    };

    /* Initialize PI controller state — fine-tunes JPEG quality within BBA level */
    pi_state_t pi = {0};
    pi_controller_reset(&pi, s_bba_levels[bba.current_level].quality, s_bba_target_bitrate[bba.current_level]);

    /* Initialize motion detection state — reduces FPS when scene is static */
    motion_state_t motion = {0};

    /* Track bytes sent in current 1-sec window for PI bitrate measurement */
    uint32_t window_bytes_sent = 0;

    /* Apply initial BBA settings */
    s_state.current_quality = s_bba_levels[bba.current_level].quality;
    s_state.current_fps     = s_bba_levels[bba.current_level].fps;

    ESP_LOGI(TAG, "Streaming task started (BBA: level=%d, Q=%d, fps=%d, PI: target=%lu B/s, motion: enabled)",
             bba.current_level, s_state.current_quality, s_state.current_fps,
             (unsigned long)s_bba_target_bitrate[bba.current_level]);

    int64_t last_stats_log = esp_timer_get_time();
    bool was_throttled     = false; /* Track throttle state for logging */

    while (s_state.running) {
        int64_t now           = esp_timer_get_time();
        TickType_t tick_start = xTaskGetTickCount();

        /* BBA + PI evaluation: every 1 second.
         * BBA (coarse): evaluates network congestion, switches quality level.
         * PI  (fine):   adjusts JPEG quality within BBA level to track target bitrate. */
        if ((now - bba.last_eval_time) >= BBA_EVAL_INTERVAL_SEC * 1000000) {
            ws_send_stats_t ws_stats;
            if (ws_manager_server_get_send_stats(&ws_stats) == ESP_OK) {
                int client_count = ws_manager_server_get_client_count();

                /* Accumulate for 10-sec log (BBA resets WS stats every 1 sec) */
                bba.accum_sent += ws_stats.total_sent;
                bba.accum_failed += ws_stats.total_failed;

                /* BBA: evaluate and possibly switch level (coarse control) */
                bba_level_t prev_level = bba.current_level;
                bba_level_t curr_level = bba_evaluate(&bba, &ws_stats, client_count);
                if (curr_level != prev_level) {
                    /* BBA level changed: update FPS and reset PI for new target */
                    s_state.current_fps = s_bba_levels[curr_level].fps;
                    pi_controller_reset(&pi, s_bba_levels[curr_level].quality, s_bba_target_bitrate[curr_level]);
                    s_state.current_quality = s_bba_levels[curr_level].quality;
                    ESP_LOGI(TAG, "PI: reset on BBA level %d->%d (Q=%d, target=%lu B/s)", prev_level, curr_level,
                             s_state.current_quality, (unsigned long)s_bba_target_bitrate[curr_level]);
                }

                /* PI: fine-tune quality within current BBA level.
                 * Skip when scene is static (FPS reduced → bitrate naturally lower,
                 * PI would incorrectly try to compensate by raising quality). */
                if (client_count > 0 && !motion.is_static) {
                    uint32_t actual_bitrate = window_bytes_sent; /* bytes in 1-sec window */
                    int base_q              = s_bba_levels[curr_level].quality;
                    int new_q = pi_controller_update(&pi, actual_bitrate, s_bba_target_bitrate[curr_level], base_q,
                                                     &s_bba_q_ranges[curr_level]);
                    if (new_q != s_state.current_quality) {
                        ESP_LOGD(TAG,
                                 "PI: Q %d -> %d (actual=%lu B/s, target=%lu B/s, "
                                 "integral=%.2f)",
                                 s_state.current_quality, new_q, (unsigned long)actual_bitrate,
                                 (unsigned long)s_bba_target_bitrate[curr_level], pi.integral);
                        s_state.current_quality = new_q;
                    }
                }

                /* Reset window and WS stats for next sampling period */
                window_bytes_sent = 0;
                ws_manager_server_reset_send_stats();
            }
            bba.last_eval_time = now;
        }

        /* Determine effective FPS based on client presence and motion state.
         * Priority: no clients → min FPS; static scene → static FPS; else BBA FPS */
        int client_count = ws_manager_server_get_client_count();
        int effective_fps;
        if (client_count == 0) {
            effective_fps = ADAPTIVE_FPS_MIN;
            if (!was_throttled) {
                ESP_LOGI(TAG, "No camera client: throttling fps %d -> %d", s_state.current_fps, ADAPTIVE_FPS_MIN);
                was_throttled = true;
            }
        } else if (motion.is_static) {
            effective_fps = MOTION_STATIC_FPS;
            was_throttled = false;
        } else {
            effective_fps = s_state.current_fps;
            was_throttled = false;
        }

        TickType_t frame_period = pdMS_TO_TICKS(1000 / effective_fps);

        /* Capture frame */
        esp_err_t ret = camera_capture_frame(s_state.config.camera);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Frame capture failed: %s", esp_err_to_name(ret));
            s_state.frames_failed++;
            vTaskDelayUntil(&tick_start, frame_period);
            continue;
        }

        /* Motion detection: check if new HIST data is available from ISR.
         * HIST is computed by ISP hardware during frame processing, so by
         * the time capture returns, the histogram for this frame should be ready.
         * Motion state affects the NEXT frame's FPS decision. */
        if (s_hist_ready) {
            uint32_t curr_hist[MOTION_HIST_BINS];
            portENTER_CRITICAL(&s_hist_spinlock);
            memcpy(curr_hist, s_latest_hist, sizeof(uint32_t) * MOTION_HIST_BINS);
            s_hist_ready = false;
            portEXIT_CRITICAL(&s_hist_spinlock);

            bool was_static = motion.is_static;
            motion_update(&motion, curr_hist);
            if (was_static != motion.is_static) {
                ESP_LOGI(TAG, "Motion: %s (SAD=%.4f, static_frames=%d)",
                         motion.is_static ? "STATIC→reduce fps" : "ACTIVE→restore fps", motion.last_sad,
                         motion.static_count);
            }
        }

        /* Encode JPEG with BBA+PI-controlled quality */
        uint32_t jpeg_size = 0;
        ret                = camera_encode_jpeg(s_state.config.camera, s_state.current_quality, &jpeg_size);
        if (ret != ESP_OK || jpeg_size == 0) {
            ESP_LOGW(TAG, "JPEG encode failed: %s", esp_err_to_name(ret));
            s_state.frames_failed++;
            vTaskDelayUntil(&tick_start, frame_period);
            continue;
        }

        /* Get JPEG buffer */
        const uint8_t *jpeg_buf = camera_get_jpeg_buffer(s_state.config.camera);
        if (!jpeg_buf) {
            ESP_LOGW(TAG, "JPEG buffer NULL");
            s_state.frames_failed++;
            vTaskDelayUntil(&tick_start, frame_period);
            continue;
        }

        /* Broadcast to /camera clients (async via httpd_queue_work).
         * BBA+PI handle congestion control — here we just update counters.
         * Send failures are tracked by ws_manager's send stats which BBA reads. */
        ret = ws_manager_server_broadcast_binary((const char *)jpeg_buf, (int)jpeg_size);
        if (ret != ESP_OK) {
            s_state.frames_failed++;
        } else {
            s_state.frames_sent++;
            s_state.frame_size_sum += jpeg_size;
            s_state.frame_size_count++;
            window_bytes_sent += jpeg_size; /* For PI bitrate measurement */
        }

        /* Periodic stats log (every 10 seconds) */
        now = esp_timer_get_time();
        if (now - last_stats_log >= 10 * 1000 * 1000) {
            uint32_t avg_size =
                (s_state.frame_size_count > 0) ? (s_state.frame_size_sum / s_state.frame_size_count) : 0;
            ESP_LOGI(TAG,
                     "Stats: sent=%u failed=%u avg_size=%u fps=%d Q=%d bba=%d "
                     "[ws: sent=%u failed=%u] [PI: actual=%lu target=%lu B/s integral=%.2f] "
                     "[motion: %s SAD=%.4f static_cnt=%d]",
                     (unsigned)s_state.frames_sent, (unsigned)s_state.frames_failed, (unsigned)avg_size, effective_fps,
                     s_state.current_quality, bba.current_level, (unsigned)bba.accum_sent, (unsigned)bba.accum_failed,
                     (unsigned long)pi.actual_bitrate_bps, (unsigned long)pi.target_bitrate_bps, pi.integral,
                     motion.is_static ? "STATIC" : "ACTIVE", motion.last_sad, motion.static_count);
            last_stats_log = now;
        }

        /* Delay to maintain effective FPS */
        vTaskDelayUntil(&tick_start, frame_period);
    }

    ESP_LOGI(TAG, "Streaming task ended");
    s_state.task_handle = NULL;
    vTaskDelete(NULL);
}

/*---------------------------------------------------------------
 * Public API
 *-------------------------------------------------------------*/
esp_err_t camera_stream_init(const camera_stream_config_t *config)
{
    if (s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!config || !config->camera) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_state, 0, sizeof(s_state));
    memcpy(&s_state.config, config, sizeof(*config));

    /* Apply defaults */
    if (s_state.config.default_quality <= 0 || s_state.config.default_quality > 100) {
        s_state.config.default_quality = 80;
    }
    if (s_state.config.default_fps <= 0 || s_state.config.default_fps > 30) {
        s_state.config.default_fps = 15;
    }
    if (s_state.config.task_stack_size <= 0) {
        s_state.config.task_stack_size = STREAM_TASK_STACK_DEFAULT;
    }
    if (s_state.config.task_priority <= 0) {
        s_state.config.task_priority = STREAM_TASK_PRIORITY_DEFAULT;
    }

    s_state.current_quality = s_state.config.default_quality;
    s_state.current_fps     = s_state.config.default_fps;

    s_state.lock = xSemaphoreCreateMutex();
    if (!s_state.lock) {
        return ESP_ERR_NO_MEM;
    }

    s_state.initialized = true;
    ESP_LOGI(TAG, "Initialized (quality=%d, fps=%d, stack=%d, prio=%d)", s_state.current_quality, s_state.current_fps,
             s_state.config.task_stack_size, s_state.config.task_priority);
    return ESP_OK;
}

void camera_stream_deinit(void)
{
    if (s_state.running) {
        camera_stream_stop();
    }
    if (s_state.lock) {
        vSemaphoreDelete(s_state.lock);
        s_state.lock = NULL;
    }
    s_state.initialized = false;
}

esp_err_t camera_stream_start(void)
{
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state.running) {
        return ESP_ERR_INVALID_STATE;
    }

    /* DEBT: [High] Camera stop→start causes capture_frame timeout (5s).
     * The CSI controller's on_get_new_trans + on_trans_finished notification
     * mechanism does not correctly recover after esp_cam_ctlr_stop() followed
     * by esp_cam_ctlr_start(). The DMA ISR stops triggering on_trans_finished
     * callbacks after restart, causing ulTaskNotifyTake() to time out.
     * Workaround: avoid calling camera_stream_stop() followed by
     * camera_stream_start() in the same session. If restart is needed,
     * perform a full deinit/init cycle instead.
     * Decision: marked as known issue (2026-07-24, user confirmed). */

    /* Start camera pipeline */
    esp_err_t ret = camera_controller_start(s_state.config.camera);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Camera start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Register HIST callback for motion detection, then enable and start.
     * ESP-IDF API lifecycle: new (fsm=INIT) → register_callback (requires INIT)
     * → enable (fsm=ENABLE) → start_continuous (fsm=CONTINUOUS).
     * camera_controller_init() only creates the controller, leaving it in INIT
     * state so we can register the callback here. */
    if (s_state.config.camera->hist_ctlr) {
        esp_isp_hist_cbs_t hist_cbs = {
            .on_statistics_done = s_hist_stats_done_cb,
        };
        ret = esp_isp_hist_register_event_callbacks(s_state.config.camera->hist_ctlr, &hist_cbs, NULL);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to register HIST callback: %s (motion detection disabled)", esp_err_to_name(ret));
        } else {
            ret = esp_isp_hist_controller_enable(s_state.config.camera->hist_ctlr);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to enable HIST controller: %s", esp_err_to_name(ret));
            } else {
                ret = esp_isp_hist_controller_start_continuous_statistics(s_state.config.camera->hist_ctlr);
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to start HIST continuous stats: %s (motion detection disabled)",
                             esp_err_to_name(ret));
                } else {
                    ESP_LOGI(TAG, "HIST callback registered, controller enabled and stats started");
                }
            }
        }
    } else {
        ESP_LOGW(TAG, "HIST controller not available, motion detection disabled");
    }

    s_state.running          = true;
    s_state.frames_sent      = 0;
    s_state.frames_failed    = 0;
    s_state.frame_size_sum   = 0;
    s_state.frame_size_count = 0;
    s_state.last_stats_time  = esp_timer_get_time();

    /* Create streaming task on core 1 with elevated priority.
     * Pinning to core 1 isolates camera streaming from display/LVGL (core 0),
     * reducing scheduling jitter. Priority 8 ensures timely frame processing
     * while still below WiFi/LWIP system tasks. */
    BaseType_t xret = xTaskCreatePinnedToCore(camera_stream_task, "cam_stream", s_state.config.task_stack_size, NULL, 8,
                                              &s_state.task_handle, 1);
    if (xret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create streaming task");
        s_state.running = false;
        camera_controller_stop(s_state.config.camera);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Streaming started");
    return ESP_OK;
}

esp_err_t camera_stream_stop(void)
{
    if (!s_state.running) {
        return ESP_ERR_INVALID_STATE;
    }

    s_state.running = false;

    /* Wait for task to exit */
    while (s_state.task_handle != NULL) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* Stop camera pipeline */
    esp_err_t ret = camera_controller_stop(s_state.config.camera);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Camera stop failed: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "Streaming stopped (sent=%u, failed=%u)", (unsigned)s_state.frames_sent,
             (unsigned)s_state.frames_failed);
    return ESP_OK;
}

bool camera_stream_is_running(void)
{
    return s_state.running;
}

esp_err_t camera_stream_set_quality(int quality)
{
    if (quality < 1 || quality > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_state.lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_state.current_quality = quality;
    xSemaphoreGive(s_state.lock);
    ESP_LOGD(TAG, "Quality set to %d", quality);
    return ESP_OK;
}

esp_err_t camera_stream_set_fps(int fps)
{
    if (fps < 1 || fps > 30) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_state.lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_state.current_fps = fps;
    xSemaphoreGive(s_state.lock);
    ESP_LOGD(TAG, "FPS set to %d", fps);
    return ESP_OK;
}

int camera_stream_get_quality(void)
{
    return s_state.current_quality;
}

int camera_stream_get_fps(void)
{
    return s_state.current_fps;
}

void camera_stream_get_stats(uint32_t *frames_sent, uint32_t *frames_failed, uint32_t *avg_frame_size,
                             float *avg_fps_actual)
{
    if (frames_sent)
        *frames_sent = s_state.frames_sent;
    if (frames_failed)
        *frames_failed = s_state.frames_failed;
    if (avg_frame_size) {
        *avg_frame_size = (s_state.frame_size_count > 0) ? (s_state.frame_size_sum / s_state.frame_size_count) : 0;
    }
    if (avg_fps_actual) {
        int64_t elapsed_us = esp_timer_get_time() - s_state.last_stats_time;
        if (elapsed_us > 0 && s_state.frames_sent > 0) {
            *avg_fps_actual = (float)s_state.frames_sent * 1000000.0f / (float)elapsed_us;
        } else {
            *avg_fps_actual = 0.0f;
        }
    }
}
