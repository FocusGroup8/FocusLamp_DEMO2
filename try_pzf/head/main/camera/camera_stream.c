/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "camera_stream.h"

#include "driver/isp_hist.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "websocket_manager.h"

#include <stdio.h>
#include <stdatomic.h>
#include <sys/param.h>
#include <string.h>

static const char *TAG = "cam_stream";

/* Monotonic frame sequence counter for frame_header text frames.
 * Incremented per captured frame; sent as JSON before each binary JPEG
 * frame so the docker main-client can track seq + timestamp and detect
 * drops / out-of-order frames. */
static uint32_t s_frame_seq = 0;

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
 * LOW:  ~10KB/frame @ 2fps → ~20KB/s
 * MID:  ~10KB/frame @ 5fps → ~50KB/s (initial baseline)
 * HIGH: ~12KB/frame @ 5fps -> ~60KB/s.
 * Quality adaptation may reduce the base quality, never raise it. The PI
 * target is feedback, not a hard traffic cap. A user ceiling <=2fps selects
 * fixed Q15 at the encoder for a controlled low-load mode. */
typedef enum { BBA_LEVEL_LOW = 0, BBA_LEVEL_MID = 1, BBA_LEVEL_HIGH = 2, BBA_LEVEL_COUNT } bba_level_t;

typedef struct {
    int quality;
    int fps;
} bba_level_config_t;

static const bba_level_config_t s_bba_levels[BBA_LEVEL_COUNT] = {
    [BBA_LEVEL_LOW]  = {.quality = 15, .fps = 2},
    [BBA_LEVEL_MID]  = {.quality = 16, .fps = 5},
    [BBA_LEVEL_HIGH] = {.quality = 18, .fps = 5},
};

/* BBA evaluation interval (seconds) */
#define BBA_EVAL_INTERVAL_SEC 1

/* Hysteresis: consecutive samples required before level switch */
#define BBA_HYSTERESIS_THRESHOLD 2

/* Minimum send attempts before allowing step-up (prevents false healthy
 * assessment when client just connected and few frames sent) */
#define BBA_MIN_SAMPLES_FOR_STEPUP 2 /* LOW produces only two samples per second. */

/* Require continuous healthy windows at the current level before step-up.
 * A bad/empty window, session change, or level change restarts this period. */
#define BBA_CLIENT_STABLE_TIME_SEC 10

/* Congestion thresholds */
#define BBA_FAILURE_RATE_HIGH 0.20f /* >20% failure rate → step down */
#define BBA_FAILURE_RATE_LOW 0.05f  /* <5% failure rate → can step up */
#define BBA_CONSEC_FAIL_HIGH 3      /* >3 consecutive failures → immediate step down */
/* [TEMP A/B 2026-08-08] threshold tuned 500ms: high send duration indicates
 * the ESP-Hosted (SDIO bridged) link is congested; BBA must step down early
 * to keep the frame rate within the link's real throughput (150KB/s at 10fps
 * exceeds the bridge under weak-signal RTT). Revert/tune after verification. */
#define BBA_SEND_DUR_HIGH_MS 500 /* >500ms avg send duration → congested */
#define BBA_SEND_DUR_LOW_MS 50   /* <50ms avg send duration → healthy */

/* BBA state */
typedef struct {
    bba_level_t current_level;
    bba_level_t pending_level;
    int hysteresis_counter;
    int64_t last_eval_time;
    /* Accumulated window stats for logging; actual sample intervals can stretch. */
    uint32_t accum_sent;
    uint32_t accum_failed;
    /* Camera-only connection tracking (anti-oscillation). */
    bool prev_camera_connected;
    uint32_t camera_session;
    uint32_t last_budget_timeouts;
    int64_t healthy_since; /* First healthy sample at the current level; 0 if interrupted */
} bba_state_t;

/* Evaluate BBA level based on WebSocket send statistics.
 * Returns the current (possibly updated) BBA level. */
static bba_level_t bba_evaluate(bba_state_t *bba, const ws_send_stats_t *stats,
                               ws_camera_client_state_t camera)
{
    int64_t now = esp_timer_get_time();

    /* Track client connection state transitions */
    if (camera.connected && (!bba->prev_camera_connected || camera.session_id != bba->camera_session)) {
        /* Session identity also catches disconnect/reconnect between samples,
         * including a new socket that reuses the previous integer fd. */
        bba->healthy_since = 0;
        bba->hysteresis_counter  = 0;
        bba->pending_level       = bba->current_level;
        ESP_LOGI(TAG, "BBA: camera session=%u connected, require %d sec continuous healthy windows",
                 (unsigned)camera.session_id, BBA_CLIENT_STABLE_TIME_SEC);
    } else if (!camera.connected && bba->prev_camera_connected) {
        bba->healthy_since = 0;
        bba->hysteresis_counter  = 0;
        bba->pending_level       = bba->current_level;
        ESP_LOGI(TAG, "BBA: camera disconnected, stable period reset");
    }
    bba->prev_camera_connected = camera.connected;
    bba->camera_session = camera.session_id;

    /* Consume durable evidence BEFORE the disconnected early return. A single
     * budget failure closes a partial frame, so waiting for consecutive bad
     * windows can miss it. Choose LOW directly rather than waiting to step
     * down through MID, retaining the baseline's response to budget failure.
     * Unsigned subtraction handles the boot counter's eventual wraparound. */
    uint32_t budget_events = stats->camera_budget_timeouts - bba->last_budget_timeouts;
    bba->last_budget_timeouts = stats->camera_budget_timeouts;
    if (budget_events > 0) {
        bba_level_t previous = bba->current_level;
        bba->current_level = BBA_LEVEL_LOW;
        bba->pending_level = BBA_LEVEL_LOW;
        bba->hysteresis_counter = 0;
        /* Even at LOW or on a zero-progress drop that keeps the connection,
         * require a new healthy interval before allowing recovery. */
        bba->healthy_since = 0;
        ESP_LOGW(TAG, "BBA: budget timeout -> LOW level=%d->%d Q=%d fps=%d "
                 "camera=%d events=%u total=%u",
                 previous, BBA_LEVEL_LOW, s_bba_levels[BBA_LEVEL_LOW].quality,
                 s_bba_levels[BBA_LEVEL_LOW].fps, camera.connected,
                 (unsigned)budget_events, (unsigned)stats->camera_budget_timeouts);
        return bba->current_level;
    }

    /* No camera: freeze at current level after consuming any timeout evidence. */
    if (!camera.connected) {
        bba->healthy_since = 0;
        bba->hysteresis_counter = 0;
        bba->pending_level      = bba->current_level;
        return bba->current_level;
    }

    /* Compute failure rate over the sampling window */
    uint32_t total          = stats->total_sent + stats->total_failed;
    float failure_rate      = (total > 0) ? (float)stats->total_failed / (float)total : 0.0f;
    int64_t avg_duration_ms = stats->avg_send_duration_us / 1000;
    bool congested = stats->consecutive_failures >= BBA_CONSEC_FAIL_HIGH ||
                     failure_rate > BBA_FAILURE_RATE_HIGH || avg_duration_ms > BBA_SEND_DUR_HIGH_MS;
    bool healthy = !congested && total >= BBA_MIN_SAMPLES_FOR_STEPUP &&
                   failure_rate < BBA_FAILURE_RATE_LOW && avg_duration_ms < BBA_SEND_DUR_LOW_MS;
    if (!healthy) {
        bba->healthy_since = 0;
    } else if (bba->healthy_since == 0) {
        bba->healthy_since = now;
    }
    int64_t healthy_sec = bba->healthy_since ? (now - bba->healthy_since) / 1000000 : 0;

    /* Determine target level based on network conditions */
    bba_level_t target = bba->current_level;

    /* Step down: severe congestion (immediate) or sustained congestion */
    if (congested) {
        if (bba->current_level > BBA_LEVEL_LOW) {
            target = (bba_level_t)(bba->current_level - 1);
        }
    }
    /* Step up: network healthy — require ALL conditions:
     * 1. Minimum sample count (prevent false healthy on few frames)
     * 2. Continuous health at this level (restart after each level switch)
     * 3. Low failure rate and fast send duration */
    else if (healthy && healthy_sec >= BBA_CLIENT_STABLE_TIME_SEC &&
             bba->current_level < BBA_LEVEL_HIGH) {
        target = (bba_level_t)(bba->current_level + 1);
    }

    /* Apply hysteresis: require N consecutive samples to confirm switch */
    if (target != bba->current_level) {
        if (target == bba->pending_level) {
            bba->hysteresis_counter++;
            if (bba->hysteresis_counter >= BBA_HYSTERESIS_THRESHOLD) {
                bba->current_level      = target;
                bba->healthy_since      = 0;
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
    [BBA_LEVEL_LOW]  = 20 * 1024,  /* 2 fps */
    [BBA_LEVEL_MID]  = 50 * 1024,  /* 5 fps */
    [BBA_LEVEL_HIGH] = 60 * 1024,  /* 5 fps, larger JPEGs; feedback, not a hard cap. */
};

/* Q value range constraints per BBA level.
 * Ranges overlap to ensure continuous Q transition across level switches. */
typedef struct {
    int q_min;
    int q_max;
} bba_q_range_t;

static const bba_q_range_t s_bba_q_ranges[BBA_LEVEL_COUNT] = {
    [BBA_LEVEL_LOW]  = {.q_min = 15, .q_max = 15},
    [BBA_LEVEL_MID]  = {.q_min = 15, .q_max = 16},
    [BBA_LEVEL_HIGH] = {.q_min = 15, .q_max = 18},
};

static uint32_t stream_target_bitrate(bba_level_t level, int fps_limit)
{
    int applied_fps = MIN(s_bba_levels[level].fps, fps_limit);
    if (fps_limit <= ADAPTIVE_FPS_MIN) {
        return 10 * 1024 * applied_fps; /* Fixed Q15: retain the LOW per-frame budget. */
    }
    return (uint32_t)((uint64_t)s_bba_target_bitrate[level] * applied_fps / s_bba_levels[level].fps);
}

/* PI controller parameters */
#define PI_KP 0.5f /* Proportional gain */
#define PI_KI 0.1f /* Integral gain */
#define PI_Q_SCALE                                                            \
    20.0f                        /* Output amplification: converts normalized \
                                  * error to Q adjustment (15% error → 1-3 Q steps) */
#define PI_INTEGRAL_LIMIT 15.0f  /* Anti-windup: integral clamp */
#define PI_EMA_ALPHA 0.15f       /* Output EMA smoothing coefficient */
#define PI_DEADZONE_RATIO 0.05f  /* ±5% dead zone */

/* PI controller state */
typedef struct {
    float integral;
    float smoothed_quality;
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
 * actual_bitrate_bps: successful JPEG payload bytes normalized by elapsed time
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
 * elapsed_sec accounts for windows stretched by capture or send blocking.
 */
static int pi_controller_update(pi_state_t *pi, uint32_t actual_bitrate_bps, uint32_t target_bitrate_bps,
                                int base_quality, const bba_q_range_t *q_range, float elapsed_sec)
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
    pi->integral += error * PI_KI * elapsed_sec;
    if (pi->integral > PI_INTEGRAL_LIMIT) {
        pi->integral = PI_INTEGRAL_LIMIT;
    } else if (pi->integral < -PI_INTEGRAL_LIMIT) {
        pi->integral = -PI_INTEGRAL_LIMIT;
    }

    /* PI output: quality adjustment (amplified by Q_SCALE for visible Q steps).
     * With PI_KP=0.5, PI_Q_SCALE=20: 15% error → 1.5 Q steps,
     * 30% error → 3 Q steps. Integral accumulates for steady-state correction. */
    float adjustment = (PI_KP * error + pi->integral) * PI_Q_SCALE;
    float target_q   = base_quality + adjustment;

    /* Clamp to BBA level Q range */
    if (target_q < q_range->q_min)
        target_q = q_range->q_min;
    if (target_q > q_range->q_max)
        target_q = q_range->q_max;

    /* EMA smoothing */
    pi->smoothed_quality = PI_EMA_ALPHA * target_q + (1.0f - PI_EMA_ALPHA) * pi->smoothed_quality;

    return (int)(pi->smoothed_quality + 0.5f);
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
static atomic_bool s_hist_ready = false;

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
    atomic_bool running;
    atomic_bool sender_stop;
    bool hist_enabled, hist_started;
    TaskHandle_t sender;
    SemaphoreHandle_t exited; /* Producer joins before sender is told to exit. */

    /* Runtime state */
    atomic_int current_quality;
    atomic_int trial_quality;       /* Q25/Q35 explicitly requested via the normal quality API */
    atomic_int trial_ceiling;       /* LOW: 15, MID: 25, HIGH: 35 */
    atomic_int current_fps;
    atomic_int fps_limit; /* User ceiling; BBA must not override it. */
    atomic_int bba_fps;

    /* Statistics */
    atomic_uint frames_failed;
    atomic_uint last_jpeg_bytes;
    int64_t last_stats_time;
} stream_state_t;

static stream_state_t s_state = {0};

typedef struct {
    uint8_t *jpeg;
    camera_frame_info_t frame;
    int64_t ready_us;
} frame_slot_t;

static frame_slot_t s_slots[2];
static int s_pending = -1, s_inflight = -1;
static portMUX_TYPE s_pipeline_lock = portMUX_INITIALIZER_UNLOCKED;
static camera_pipeline_stats_t s_pipeline;
/* Only public lifecycle/configuration operations take this gate. Neither
 * worker takes it, so the owner may join both workers while holding it. */
static atomic_flag s_lifecycle = ATOMIC_FLAG_INIT;
static portMUX_TYPE s_control_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_control_busy, s_control_closing = true;
static esp_err_t s_control_result = ESP_OK;
static void (*s_control_completion)(esp_err_t result, void *context);
static void *s_control_context;

static void lifecycle_enter(void)
{
    while (atomic_flag_test_and_set(&s_lifecycle)) vTaskDelay(1);
}

void camera_stream_pipeline_stats(camera_pipeline_stats_t *out)
{
    portENTER_CRITICAL(&s_pipeline_lock);
    *out = s_pipeline;
    portEXIT_CRITICAL(&s_pipeline_lock);
}

static void publish_frame(camera_frame_info_t frame, const uint8_t *jpeg)
{
    /* One producer. Removing pending before copying makes the slot private.
     * Sender can only claim a published slot, never this one mid-copy. */
    portENTER_CRITICAL(&s_pipeline_lock);
    int index = s_inflight == 0 ? 1 : 0;
    if (s_pending >= 0) index = s_pending;
    if (!s_slots[index].jpeg) {
        ++s_pipeline.resource_drops;
        portEXIT_CRITICAL(&s_pipeline_lock);
        return;
    }
    if (s_pending >= 0) {
        if (s_slots[index].frame.session == frame.session) ++s_pipeline.replaced;
        else ++s_pipeline.stale;
        s_pending = -1;
    }
    portEXIT_CRITICAL(&s_pipeline_lock);

    frame_slot_t *slot = &s_slots[index];
    memcpy(slot->jpeg, jpeg, frame.length);
    slot->frame = frame;
    slot->ready_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_pipeline_lock);
    s_pending = index;
    ++s_pipeline.enqueued;
    portEXIT_CRITICAL(&s_pipeline_lock);
    xTaskNotifyGive(s_state.sender);
}

static void send_pending_frame(void)
{
    portENTER_CRITICAL(&s_pipeline_lock);
    int index = s_pending;
    if (index < 0 || !s_state.running) {
        portEXIT_CRITICAL(&s_pipeline_lock);
        return;
    }
    s_inflight = index;
    s_pending = -1;
    portEXIT_CRITICAL(&s_pipeline_lock);

    const frame_slot_t *slot = &s_slots[index];
    const camera_frame_info_t f = slot->frame;
    camera_delivery_t d = {.frame = f, .ready_wait_us = esp_timer_get_time() - slot->ready_us};
    char header[96];
    int n = snprintf(header, sizeof(header),
                     "{\"type\":\"frame_header\",\"seq\":%u,\"capture_us\":%lld}",
                     (unsigned)f.seq, (long long)f.capture_us);
    ws_camera_send_result_t wire = {0};
    bool valid = n > 0 && (size_t)n < sizeof(header);
    esp_err_t ret = valid ? ws_manager_server_send_camera_pair(f.session, header, n,
                               (const char *)slot->jpeg, f.length, &wire) : ESP_ERR_INVALID_SIZE;
    d.lock_wait_us = wire.lock_wait_us;
    d.network_us = wire.network_us;
    d.wire_bytes = wire.wire_bytes;
    d.socket_errno = wire.socket_errno;
    d.residence_us = esp_timer_get_time() - f.capture_us;
    uint32_t stack = uxTaskGetStackHighWaterMark(NULL);
    portENTER_CRITICAL(&s_pipeline_lock);
    if (ret == ESP_OK) {
        d.reason = "sent";
        ++s_pipeline.sent;
        s_pipeline.jpeg_bytes += f.length;
    } else if (ret == ESP_ERR_NOT_FOUND) {
        d.reason = "stale_session";
        ++s_pipeline.stale;
    } else if (!valid) {
        d.reason = "metadata";
        ++s_pipeline.metadata_drops;
        ++s_state.frames_failed;
    } else {
        d.reason = wire.budget_timeout ? (wire.wire_bytes ? "partial_timeout" : "zero_timeout") :
                   ret == ESP_ERR_INVALID_STATE ? "server_stopped" : "connection_error";
        ++s_pipeline.network_failures;
        ++s_state.frames_failed;
    }
    s_pipeline.last_attempt = d;
    if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) s_pipeline.last_failure = d;
    s_pipeline.sender_stack_min = stack;
    s_inflight = -1;
    portEXIT_CRITICAL(&s_pipeline_lock);
}

static void camera_sender_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_state.sender_stop) break;
        send_pending_frame();
        /* A saturated mailbox can keep this priority-7 task ready forever.
         * Release the CPU after releasing the camera mutex so priority-5
         * HTTPD can handle Pong, Close, admission and identity requests. */
        vTaskDelay(1);
    }
    xSemaphoreGive(s_state.exited);
    vTaskDelete(NULL);
}

static void wait_frame_period(TickType_t start, TickType_t period)
{
    TickType_t elapsed = xTaskGetTickCount() - start;
    vTaskDelay(elapsed < period ? period - elapsed : 1);
}

/*---------------------------------------------------------------
 * Internal: Streaming task
 *-------------------------------------------------------------*/
static void camera_stream_task(void *arg)
{
    /* A restarted stream observes new failures, not old boot-history events. */
    ws_send_stats_t initial_ws_stats = {0};
    (void)ws_manager_server_take_send_stats(&initial_ws_stats);
    /* Initialize BBA state — start at MID level (current baseline) */
    bba_state_t bba = {
        .current_level       = BBA_LEVEL_MID,
        .pending_level       = BBA_LEVEL_MID,
        .hysteresis_counter  = 0,
        .last_eval_time      = esp_timer_get_time(),
        .accum_sent          = 0,
        .accum_failed        = 0,
        .prev_camera_connected = false,
        .camera_session = 0,
        .last_budget_timeouts = initial_ws_stats.camera_budget_timeouts,
        .healthy_since       = 0,
    };

    /* Initialize PI controller state — fine-tunes JPEG quality within BBA level */
    pi_state_t pi = {0};
    int applied_fps_limit = atomic_load(&s_state.fps_limit);
    int initial_quality = applied_fps_limit <= ADAPTIVE_FPS_MIN ? 15 : s_bba_levels[bba.current_level].quality;
    pi_controller_reset(&pi, initial_quality, stream_target_bitrate(bba.current_level, applied_fps_limit));

    /* Motion remains diagnostic; static-scene FPS throttling is disabled. */
    motion_state_t motion = {0};

    /* Successful JPEG payload accepted by the socket, not over-the-air bytes.
     * This clock advances only when its matching byte counter is cleared. */
    int64_t window_start_us = esp_timer_get_time();

    /* Apply initial BBA settings */
    s_state.current_quality = initial_quality;
    s_state.trial_ceiling = 25;
    s_state.bba_fps         = s_bba_levels[bba.current_level].fps;
    s_state.current_fps     = MIN(s_bba_levels[bba.current_level].fps, applied_fps_limit);

    ESP_LOGI(TAG, "Streaming task started (BBA: level=%d, Q=%d, fps=%d, PI: target=%lu B/s, motion: enabled)",
             bba.current_level, s_state.current_quality, s_state.current_fps,
             (unsigned long)pi.target_bitrate_bps);
    ESP_LOGI(TAG, "BBA policy: camera-only sessions, budget timeout -> LOW, continuous healthy recovery=%ds per level",
             BBA_CLIENT_STABLE_TIME_SEC);
    ESP_LOGI(TAG, "Camera profile: conservative-5fps-v1, cap<=2 -> fixed Q15, adaptive Q ceilings=15/16/18");

    int64_t last_stats_log = esp_timer_get_time();
    int64_t diag_last_loop_us = 0, diag_loop_gap_us = 0;
    int64_t diag_capture_us = 0, diag_encode_us = 0;
    bool was_throttled     = false; /* Track throttle state for logging */

    while (s_state.running) {
        int64_t now           = esp_timer_get_time();
        TickType_t tick_start = xTaskGetTickCount();
        int64_t diag_loop_now_us = esp_timer_get_time();
        if (diag_last_loop_us) diag_loop_gap_us = MAX(diag_loop_gap_us, diag_loop_now_us - diag_last_loop_us);
        diag_last_loop_us = diag_loop_now_us;
        int fps_limit = atomic_load(&s_state.fps_limit);
        bool cap_changed = fps_limit != applied_fps_limit;

        /* BBA + PI evaluation: target interval 1 second; blocking may stretch it.
         * BBA (coarse): evaluates network congestion, switches quality level.
         * PI  (fine):   adjusts JPEG quality within BBA level to track target bitrate. */
        if (cap_changed || (now - bba.last_eval_time) >= BBA_EVAL_INTERVAL_SEC * 1000000) {
            ws_send_stats_t ws_stats;
            if (ws_manager_server_take_send_stats(&ws_stats) == ESP_OK) {
                uint64_t window_bytes_sent = ws_stats.jpeg_bytes;
                ws_camera_client_state_t camera = ws_manager_server_get_camera_state();

                /* Accumulate window counters before resetting them. */
                bba.accum_sent += ws_stats.total_sent;
                bba.accum_failed += ws_stats.total_failed;

                /* BBA: evaluate and possibly switch level (coarse control) */
                if (cap_changed) {
                    bba.healthy_since = 0;
                    bba.hysteresis_counter = 0;
                    bba.pending_level = bba.current_level;
                }
                bool budget_changed = ws_stats.camera_budget_timeouts != bba.last_budget_timeouts;
                bba_level_t prev_level = bba.current_level;
                bba_level_t curr_level = bba_evaluate(&bba, &ws_stats, camera);
                s_state.trial_ceiling = curr_level == BBA_LEVEL_LOW ? 15 :
                                        curr_level == BBA_LEVEL_MID ? 25 : 35;
                applied_fps_limit = fps_limit;
                s_state.bba_fps = s_bba_levels[curr_level].fps;
                s_state.current_fps = MIN(s_bba_levels[curr_level].fps, fps_limit);
                uint32_t target_bitrate = stream_target_bitrate(curr_level, fps_limit);
                bool fixed_low = fps_limit <= ADAPTIVE_FPS_MIN;
                if (curr_level != prev_level || cap_changed || budget_changed) {
                    /* Discard the transition sample: it was collected under
                     * the previous cadence/target, not the newly applied one. */
                    int base_q = fixed_low ? 15 : s_bba_levels[curr_level].quality;
                    pi_controller_reset(&pi, base_q, target_bitrate);
                    s_state.current_quality = base_q;
                    ESP_LOGI(TAG, "PI: reset level=%d->%d fps_cap=%d mode=%s (Q=%d target=%lu B/s)",
                             prev_level, curr_level, fps_limit, fixed_low ? "fixed-low" : "adaptive",
                             base_q, (unsigned long)target_bitrate);
                } else {
                    int64_t elapsed_us = now - window_start_us;
                    if (elapsed_us > 0) {
                        uint32_t actual_bitrate = (uint32_t)(window_bytes_sent * 1000000ULL / (uint64_t)elapsed_us);
                        pi.actual_bitrate_bps = actual_bitrate;
                        pi.target_bitrate_bps = target_bitrate;
                        /* Static scenes still use the selected cadence, so
                         * they need the same PI feedback as moving scenes. */
                        if (camera.connected && !fixed_low && (ws_stats.total_sent || ws_stats.total_failed)) {
                            int new_q = pi_controller_update(&pi, actual_bitrate, target_bitrate,
                                                             s_bba_levels[curr_level].quality,
                                                             &s_bba_q_ranges[curr_level],
                                                             (float)elapsed_us / 1000000.0f);
                            s_state.current_quality = new_q;
                        } else if (fixed_low) {
                            s_state.current_quality = 15;
                        }
                    }
                }

                /* The window was consumed atomically above. */
                window_start_us = now;
            } else {
                /* An unobserved window cannot extend continuous health. Do
                 * not reset the PI byte counter or its clock on this path. */
                bba.healthy_since = 0;
                bba.hysteresis_counter = 0;
                bba.pending_level = bba.current_level;
            }
            bba.last_eval_time = now;
        }

        /* Only /camera consumes JPEGs; /algo or /mcp must not keep full FPS. */
        ws_camera_client_state_t camera = ws_manager_server_get_camera_state();
        int effective_fps;
        if (!camera.connected) {
            effective_fps = MIN(ADAPTIVE_FPS_MIN, (int)s_state.fps_limit);
            if (!was_throttled) {
                ESP_LOGI(TAG, "No camera client: throttling fps %d -> %d", s_state.current_fps, ADAPTIVE_FPS_MIN);
                was_throttled = true;
            }
        } else {
            /* Read the BBA cadence directly so a concurrent FPS setter cannot
             * leave a stale cached cadence above a newly reduced BBA level. */
            effective_fps = MIN((int)s_state.bba_fps, (int)s_state.fps_limit);
            was_throttled = false;
        }

        TickType_t frame_period = pdMS_TO_TICKS(1000 / effective_fps);

        /* Capture frame */
        int64_t diag_stage_start_us = esp_timer_get_time();
        esp_err_t ret = camera_capture_frame(s_state.config.camera);
        int64_t capture_us = esp_timer_get_time();
        uint32_t capture_cost = capture_us - diag_stage_start_us;
        diag_capture_us = MAX(diag_capture_us, capture_cost);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Frame capture failed: %s", esp_err_to_name(ret));
            s_state.frames_failed++;
            wait_frame_period(tick_start, frame_period);
            continue;
        }

        /* Motion detection: check if new HIST data is available from ISR.
         * HIST is computed by ISP hardware during frame processing, so by
         * the time capture returns, the histogram for this frame should be ready.
         * Motion statistics are diagnostic and do not change FPS or PI. */
        if (s_hist_ready) {
            uint32_t curr_hist[MOTION_HIST_BINS];
            portENTER_CRITICAL(&s_hist_spinlock);
            memcpy(curr_hist, s_latest_hist, sizeof(uint32_t) * MOTION_HIST_BINS);
            s_hist_ready = false;
            portEXIT_CRITICAL(&s_hist_spinlock);

            /* Motion state-change log is silenced: the "STATIC->reduce fps"
             * message would be misleading because static-scene FPS throttling
             * is disabled (see effective_fps logic above). SAD/static_cnt
             * remain visible in the periodic Stats line. */
            // bool was_static = motion.is_static;
            motion_update(&motion, curr_hist);
            // if (was_static != motion.is_static) {
            //     ESP_LOGI(TAG, "Motion: %s (SAD=%.4f, static_frames=%d)",
            //              motion.is_static ? "STATIC→reduce fps" : "ACTIVE→restore fps", motion.last_sad,
            //              motion.static_count);
            // }
        }

        /* Encode JPEG with BBA+PI-controlled quality */
        uint32_t jpeg_size = 0;
        int frame_fps_limit = atomic_load(&s_state.fps_limit);
        bool frame_fixed_low = frame_fps_limit <= ADAPTIVE_FPS_MIN;
        int frame_quality = frame_fixed_low ? 15 : camera_stream_get_quality();
        effective_fps = MIN(effective_fps, frame_fps_limit);
        frame_period = pdMS_TO_TICKS(1000 / effective_fps);
        diag_stage_start_us = esp_timer_get_time();
        ret = camera_encode_jpeg(s_state.config.camera, frame_quality, &jpeg_size);
        uint32_t encode_cost = esp_timer_get_time() - diag_stage_start_us;
        diag_encode_us = MAX(diag_encode_us, encode_cost);
        if (ret != ESP_OK || jpeg_size == 0) {
            ESP_LOGW(TAG, "JPEG encode failed: %s", esp_err_to_name(ret));
            s_state.frames_failed++;
            wait_frame_period(tick_start, frame_period);
            continue;
        }

        /* Get JPEG buffer */
        s_state.last_jpeg_bytes = jpeg_size;
        const uint8_t *jpeg_buf = camera_get_jpeg_buffer(s_state.config.camera);
        if (!jpeg_buf) {
            ESP_LOGW(TAG, "JPEG buffer NULL");
            s_state.frames_failed++;
            wait_frame_period(tick_start, frame_period);
            continue;
        }

        camera_frame_info_t frame = {.seq = ++s_frame_seq, .session = camera.session_id,
            .length = jpeg_size, .capture_us = capture_us, .capture_cost_us = capture_cost,
            .encode_us = encode_cost};
        ret = ESP_OK;
        if (camera.connected && s_state.running) {
            if (jpeg_size <= WS_CAMERA_MAX_JPEG) {
                publish_frame(frame, jpeg_buf);
            } else {
                ret = ESP_ERR_INVALID_SIZE;
                ++s_state.frames_failed;
                portENTER_CRITICAL(&s_pipeline_lock);
                ++s_pipeline.oversize;
                s_pipeline.last_failure = (camera_delivery_t){.frame = frame, .reason = "oversize"};
                portEXIT_CRITICAL(&s_pipeline_lock);
            }
        }
        uint32_t work_us = esp_timer_get_time() - now;
        uint32_t stack_min = uxTaskGetStackHighWaterMark(NULL);
        portENTER_CRITICAL(&s_pipeline_lock);
        s_pipeline.produced = frame;
        s_pipeline.producer_work_us = work_us;
        s_pipeline.producer_stack_min = stack_min;
        portEXIT_CRITICAL(&s_pipeline_lock);
        /* Oversize is rejected before the socket statistics are updated. Let
         * the next frame use LOW and require the normal healthy recovery. */
        if (ret == ESP_ERR_INVALID_SIZE && frame_quality > 18 && s_state.trial_quality) {
            bba.current_level = bba.pending_level = BBA_LEVEL_LOW;
            bba.healthy_since = 0;
            bba.hysteresis_counter = 0;
            s_state.bba_fps = s_state.current_fps = ADAPTIVE_FPS_MIN;
            s_state.trial_ceiling = s_state.current_quality = 15;
            pi_controller_reset(&pi, 15, stream_target_bitrate(BBA_LEVEL_LOW, frame_fps_limit));
        }

        /* Periodic stats log (every 10 seconds) */
        now = esp_timer_get_time();
        if (now - last_stats_log >= 10 * 1000 * 1000) {
            uint32_t avg_size, completed;
            camera_stream_get_stats(&completed, NULL, &avg_size, NULL);
            /* Fetch latest ws stats to expose link send duration (throughput
             * diagnostic: avg_size/avg_dur ≈ effective link throughput). */
            int64_t ws_avg_dur_ms  = 0;
            int64_t ws_last_dur_ms = 0;
            ws_send_stats_t ws_stats_log = {0};
            if (ws_manager_server_get_send_stats(&ws_stats_log) == ESP_OK) {
                ws_avg_dur_ms  = ws_stats_log.avg_send_duration_us / 1000;
                ws_last_dur_ms = ws_stats_log.last_send_duration_us / 1000;
            }
            ws_camera_client_state_t camera_log = ws_manager_server_get_camera_state();
            ESP_LOGI(TAG,
                     "Stats: sent=%u failed=%u avg_size=%u target_fps=%d actualQ=%d mode=%s bba=%d "
                     "[ws: sent=%u failed=%u avg_dur=%lldms last=%lldms] "
                     "[camera: connected=%d session=%u budget_total=%u] "
                     "[PI: actual=%lu target=%lu B/s integral=%.2f] "
                     "[motion: %s SAD=%.4f static_cnt=%d] "
                     "[stage_max_ms: loop_gap=%lld capture=%lld encode=%lld]",
                     (unsigned)completed, (unsigned)s_state.frames_failed, (unsigned)avg_size, effective_fps,
                     frame_quality, frame_fixed_low ? "fixed-low" : s_state.trial_quality ? "clarity" : "adaptive", bba.current_level,
                     (unsigned)bba.accum_sent, (unsigned)bba.accum_failed,
                     (long long)ws_avg_dur_ms, (long long)ws_last_dur_ms,
                     camera_log.connected, (unsigned)camera_log.session_id,
                     (unsigned)ws_stats_log.camera_budget_timeouts, (unsigned long)pi.actual_bitrate_bps,
                     (unsigned long)pi.target_bitrate_bps, pi.integral, motion.is_static ? "STATIC" : "ACTIVE",
                     motion.last_sad, motion.static_count,
                     (long long)(diag_loop_gap_us / 1000), (long long)(diag_capture_us / 1000),
                     (long long)(diag_encode_us / 1000));
            diag_loop_gap_us = diag_capture_us = diag_encode_us = 0;
            last_stats_log = now;
        }

        /* Delay to maintain effective FPS */
        wait_frame_period(tick_start, frame_period);
    }

    ESP_LOGI(TAG, "Streaming task ended");
    xSemaphoreGive(s_state.exited);
    vTaskDelete(NULL);
}

/*---------------------------------------------------------------
 * Public API
 *-------------------------------------------------------------*/
static esp_err_t stream_init_locked(const camera_stream_config_t *config)
{
    if (s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!config || !config->camera) {
        return ESP_ERR_INVALID_ARG;
    }

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
    s_state.trial_quality = 0;
    s_state.bba_fps = s_state.config.default_fps;
    s_state.trial_ceiling   = 25;
    s_state.current_fps     = s_state.config.default_fps;
    s_state.fps_limit       = s_state.config.default_fps;

    s_state.initialized = true;
    ESP_LOGI(TAG, "Initialized (quality=%d, fps=%d, stack=%d, prio=%d)", s_state.current_quality, s_state.current_fps,
             s_state.config.task_stack_size, s_state.config.task_priority);
    return ESP_OK;
}

static void free_sender_resources(void)
{
    for (unsigned i = 0; i < 2; ++i) {
        heap_caps_free(s_slots[i].jpeg);
        s_slots[i].jpeg = NULL;
    }
    if (s_state.exited) vSemaphoreDelete(s_state.exited);
    s_state.exited = NULL;
}

static void stop_hist_statistics(void)
{
    if (s_state.hist_started) {
        esp_isp_hist_controller_stop_continuous_statistics(s_state.config.camera->hist_ctlr);
        s_state.hist_started = false;
    }
    if (s_state.hist_enabled) {
        esp_isp_hist_controller_disable(s_state.config.camera->hist_ctlr);
        s_state.hist_enabled = false;
    }
}

static esp_err_t stream_start_locked(void)
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

    s_state.exited = xSemaphoreCreateBinary();
    for (unsigned i = 0; i < 2; ++i)
        s_slots[i].jpeg = heap_caps_malloc(WS_CAMERA_MAX_JPEG, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_state.exited || !s_slots[0].jpeg || !s_slots[1].jpeg) {
        free_sender_resources();
        return ESP_ERR_NO_MEM;
    }

    /* Start camera pipeline */
    esp_err_t ret = camera_controller_start(s_state.config.camera);
    if (ret != ESP_OK) {
        free_sender_resources();
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
                s_state.hist_enabled = true;
                ret = esp_isp_hist_controller_start_continuous_statistics(s_state.config.camera->hist_ctlr);
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to start HIST continuous stats: %s (motion detection disabled)",
                             esp_err_to_name(ret));
                } else {
                    s_state.hist_started = true;
                    ESP_LOGI(TAG, "HIST callback registered, controller enabled and stats started");
                }
            }
        }
    } else {
        ESP_LOGW(TAG, "HIST controller not available, motion detection disabled");
    }

    s_state.running          = true;
    s_state.last_jpeg_bytes  = 0;
    s_state.frames_failed    = 0;
    s_state.sender_stop = false;
    portENTER_CRITICAL(&s_pipeline_lock);
    s_pipeline = (camera_pipeline_stats_t){0};
    s_state.last_stats_time = esp_timer_get_time();
    s_pending = s_inflight = -1;
    portEXIT_CRITICAL(&s_pipeline_lock);

    /* Create streaming task on core 1 with elevated priority.
     * Pinning to core 1 isolates camera streaming from display/LVGL (core 0),
     * reducing scheduling jitter. Priority 8 ensures timely frame processing
     * while still below WiFi/LWIP system tasks. */
    BaseType_t xret = xTaskCreatePinnedToCore(camera_sender_task, "cam_send", 4096, NULL, 7,
                                              &s_state.sender, 1);
    if (xret == pdPASS) {
        xret = xTaskCreatePinnedToCore(camera_stream_task, "cam_stream", s_state.config.task_stack_size,
                                       NULL, 8, NULL, 1);
        if (xret != pdPASS) {
            s_state.sender_stop = true;
            xTaskNotifyGive(s_state.sender);
            xSemaphoreTake(s_state.exited, portMAX_DELAY);
        }
    }
    if (xret != pdPASS) {
        s_state.running = false;
        s_state.sender = NULL;
        stop_hist_statistics();
        camera_controller_stop(s_state.config.camera);
        free_sender_resources();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Streaming started");
    return ESP_OK;
}

static esp_err_t stream_stop_locked(void)
{
    if (!s_state.running) {
        return ESP_ERR_INVALID_STATE;
    }

    s_state.running = false;

    xSemaphoreTake(s_state.exited, portMAX_DELAY); /* Producer cannot notify sender past here. */
    s_state.sender_stop = true;
    xTaskNotifyGive(s_state.sender);
    xSemaphoreTake(s_state.exited, portMAX_DELAY); /* Last network write returned. */
    s_state.sender = NULL;
    portENTER_CRITICAL(&s_pipeline_lock);
    if (s_pending >= 0) ++s_pipeline.stopped;
    s_pending = -1;
    portEXIT_CRITICAL(&s_pipeline_lock);
    free_sender_resources();

    stop_hist_statistics();
    /* Stop camera pipeline */
    esp_err_t ret = camera_controller_stop(s_state.config.camera);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Camera stop failed: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "Streaming stopped (sent=%u, failed=%u)", (unsigned)s_pipeline.sent,
             (unsigned)s_state.frames_failed);
    return ESP_OK;
}

esp_err_t camera_stream_init(const camera_stream_config_t *config)
{
    lifecycle_enter();
    esp_err_t ret = stream_init_locked(config);
    if (ret == ESP_OK) {
        portENTER_CRITICAL(&s_control_lock);
        s_control_closing = false;
        s_control_result = ESP_OK;
        portEXIT_CRITICAL(&s_control_lock);
    }
    atomic_flag_clear(&s_lifecycle);
    return ret;
}

esp_err_t camera_stream_start(void)
{
    lifecycle_enter();
    esp_err_t ret = stream_start_locked();
    atomic_flag_clear(&s_lifecycle);
    return ret;
}

esp_err_t camera_stream_stop(void)
{
    lifecycle_enter();
    esp_err_t ret = stream_stop_locked();
    atomic_flag_clear(&s_lifecycle);
    return ret;
}

void camera_stream_deinit(void)
{
    portENTER_CRITICAL(&s_control_lock);
    s_control_closing = true;
    bool busy = s_control_busy;
    portEXIT_CRITICAL(&s_control_lock);
    while (busy) {
        vTaskDelay(1);
        camera_stream_get_control_status(&busy, NULL);
    }
    lifecycle_enter();
    if (s_state.running) stream_stop_locked();
    s_state.initialized = false;
    atomic_flag_clear(&s_lifecycle);
}

void camera_stream_get_control_status(bool *busy, esp_err_t *last_result)
{
    portENTER_CRITICAL(&s_control_lock);
    if (busy) *busy = s_control_busy;
    if (last_result) *last_result = s_control_result;
    portEXIT_CRITICAL(&s_control_lock);
}

static void camera_control_task(void *arg)
{
    bool start = (bool)(uintptr_t)arg;
    lifecycle_enter();
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (s_state.initialized) {
        result = ESP_OK;
        if (s_state.running != start) {
            if (start) result = stream_start_locked();
            else result = stream_stop_locked();
        }
    }
    atomic_flag_clear(&s_lifecycle);
    /* The lifecycle owner has finished touching resources before reopening
     * admission. A failed operation is observable without retrying it. */
    portENTER_CRITICAL(&s_control_lock);
    s_control_result = result;
    void (*completion)(esp_err_t, void *) = s_control_completion;
    void *context = s_control_context;
    portEXIT_CRITICAL(&s_control_lock);
    if (completion) completion(result, context);
    portENTER_CRITICAL(&s_control_lock);
    s_control_completion = NULL;
    s_control_context = NULL;
    s_control_busy = false;
    portEXIT_CRITICAL(&s_control_lock);
    vTaskDelete(NULL);
}

esp_err_t camera_stream_request_control(bool start,
                                       void (*completion)(esp_err_t result, void *context), void *context)
{
    portENTER_CRITICAL(&s_control_lock);
    if (s_control_closing || s_control_busy) {
        portEXIT_CRITICAL(&s_control_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_control_busy = true;
    s_control_completion = completion;
    s_control_context = context;
    portEXIT_CRITICAL(&s_control_lock);
    if (xTaskCreate(camera_control_task, "cam_control", 4096,
                    (void *)(uintptr_t)start, 5, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_control_lock);
        s_control_result = ESP_ERR_NO_MEM;
        s_control_completion = NULL;
        s_control_context = NULL;
        s_control_busy = false;
        portEXIT_CRITICAL(&s_control_lock);
        return ESP_ERR_NO_MEM;
    }
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
    if (atomic_flag_test_and_set(&s_lifecycle)) return ESP_ERR_INVALID_STATE;
    if (!s_state.initialized) {
        atomic_flag_clear(&s_lifecycle);
        return ESP_ERR_INVALID_STATE;
    }
    int applied_quality = s_state.fps_limit <= ADAPTIVE_FPS_MIN ? 15 : quality;
    s_state.current_quality = applied_quality;
    s_state.trial_quality = (quality == 25 || quality == 35) ? quality : 0;
    atomic_flag_clear(&s_lifecycle);
    ESP_LOGD(TAG, "Quality requested=%d applied=%d", quality, applied_quality);
    return ESP_OK;
}

esp_err_t camera_stream_set_fps(int fps)
{
    if (fps < 1 || fps > 30) {
        return ESP_ERR_INVALID_ARG;
    }
    if (atomic_flag_test_and_set(&s_lifecycle)) return ESP_ERR_INVALID_STATE;
    if (!s_state.initialized) {
        atomic_flag_clear(&s_lifecycle);
        return ESP_ERR_INVALID_STATE;
    }
    s_state.fps_limit = fps;
    s_state.current_fps = MIN((int)s_state.bba_fps, fps);
    if (fps <= ADAPTIVE_FPS_MIN) {
        s_state.current_quality = 15;
    }
    atomic_flag_clear(&s_lifecycle);
    ESP_LOGD(TAG, "FPS set to %d", fps);
    return ESP_OK;
}

int camera_stream_get_quality(void)
{
    int trial = atomic_load(&s_state.trial_quality);
    int ceiling = atomic_load(&s_state.trial_ceiling);
    return s_state.fps_limit <= ADAPTIVE_FPS_MIN ? 15 :
           trial ? MIN(trial, ceiling) : (int)s_state.current_quality;
}

int camera_stream_get_trial_quality(void)
{
    return atomic_load(&s_state.trial_quality);
}

uint32_t camera_stream_last_jpeg_bytes(void)
{
    return atomic_load(&s_state.last_jpeg_bytes);
}

int camera_stream_get_fps(void)
{
    return MIN((int)s_state.current_fps, (int)s_state.fps_limit);
}

void camera_stream_get_stats(uint32_t *frames_sent, uint32_t *frames_failed, uint32_t *avg_frame_size,
                             float *avg_fps_actual)
{
    portENTER_CRITICAL(&s_pipeline_lock);
    uint64_t bytes = s_pipeline.jpeg_bytes;
    uint32_t sent = s_pipeline.sent;
    int64_t started = s_state.last_stats_time;
    portEXIT_CRITICAL(&s_pipeline_lock);
    if (frames_sent)
        *frames_sent = sent;
    if (frames_failed)
        *frames_failed = s_state.frames_failed;
    if (avg_frame_size) {
        *avg_frame_size = sent ? bytes / sent : 0;
    }
    if (avg_fps_actual) {
        int64_t elapsed_us = esp_timer_get_time() - started;
        if (elapsed_us > 0 && sent > 0) {
            *avg_fps_actual = (float)sent * 1000000.0f / (float)elapsed_us;
        } else {
            *avg_fps_actual = 0.0f;
        }
    }
}
