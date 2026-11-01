#include "controller_modes.h"

#include <stddef.h>
#include <string.h>
#include "esp_random.h"
#include "focus_clock.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "controller_modes";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized;
static bool s_initializing;
static bool s_voice_active;
static app_state_t s_mode = APP_STATE_IDLE;
static uint32_t s_duration_sec;
static focus_clock_t s_focus;
static uint32_t s_boot_id, s_input_token;
static int64_t s_last_us;
static int64_t s_last_summary_us;
static bool s_completed;
static uint32_t s_revision;
static esp_timer_handle_t s_timer;

/* These setters take only the short device_state critical section. Mirror
 * writes stay inside s_lock so concurrent commands cannot reorder them.
 * Lock order is modes -> device_state; no callbacks are invoked here. */
static void mirror_locked(void)
{
    (void)device_state_set_app_state(s_mode);
    (void)device_state_set_voice_active(s_voice_active);
}

const char *controller_modes_name(app_state_t mode)
{
    switch (mode) {
    case APP_STATE_IDLE: return "idle";
    case APP_STATE_FOCUS: return "focus";
    case APP_STATE_COMPANION: return "companion";
    default: return "unknown";
    }
}

/* Account for the interval under its OLD voice state before changing that
 * state. Paused intervals update last_us without consuming session time. */
static bool advance_locked(int64_t now_us)
{
    s_last_us = now_us;
    bool was_completed = s_focus.completed;
    focus_clock_settle(&s_focus, now_us);
    s_completed = s_focus.completed;
    if (!was_completed && s_completed) ++s_revision;
    return !was_completed && s_completed;
}

const char *controller_modes_pause_name(unsigned reason)
{
    static const char *names[] = {"waiting", "running", "distracted", "invalid", "data_stale", "voice", "completed"};
    return reason < sizeof(names)/sizeof(names[0]) ? names[reason] : "invalid";
}

static void snapshot_locked(controller_modes_status_t *out)
{
    int64_t remaining_us = s_focus.remaining_us;
    *out = (controller_modes_status_t) {
        .mode = s_mode,
        .duration_sec = s_duration_sec,
        .remaining_sec = remaining_us > 0 ? (uint32_t)((remaining_us + 999999) / 1000000) : 0,
        .voice_active = s_voice_active,
        .paused = s_mode == APP_STATE_FOCUS && !s_completed && (s_voice_active || !s_focus.rate_half),
        .uptime_ms = (uint32_t)(esp_timer_get_time()/1000),
        .duration_ms = s_duration_sec * 1000,
        .remaining_ms = (uint32_t)((remaining_us + 999) / 1000),
        .controller_boot_id = s_boot_id, .session_id = s_focus.session,
        .seq = ++s_focus.seq, .engagement_token = s_input_token,
        .rate_half = s_mode != APP_STATE_FOCUS || s_voice_active || s_completed ? 0 : s_focus.rate_half,
        .pause_reason = s_completed ? FOCUS_DONE : s_voice_active ? FOCUS_VOICE : s_focus.reason,
        .completed = s_completed,
        .revision = s_revision,
    };
}

static void log_completed(bool completed, uint32_t duration_sec)
{
    if (completed) {
        ESP_LOGI(TAG, "Focus completed: duration=%u s, remaining=0, scope=state_only",
                 (unsigned)duration_sec);
    }
}

static void timer_callback(void *arg)
{
    (void)arg;
    controller_modes_status_t status;
    bool summary = false;
    bool completed = false;

    portENTER_CRITICAL(&s_lock);
    if (!s_initialized) {
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    int64_t now_us = esp_timer_get_time();
    completed = advance_locked(now_us);
    snapshot_locked(&status);
    if (now_us - s_last_summary_us >= 30000000) {
        s_last_summary_us = now_us;
        summary = true;
    }
    portEXIT_CRITICAL(&s_lock);

    log_completed(completed, status.duration_sec);
    if (summary) {
        ESP_LOGI(TAG, "Mode summary: mode=%s duration=%u remaining=%u voice=%d paused=%d completed=%d scope=state_engine",
                 controller_modes_name(status.mode), (unsigned)status.duration_sec,
                 (unsigned)status.remaining_sec, status.voice_active,
                 status.paused, status.completed);
    }
}

esp_err_t controller_modes_init(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_initialized) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_OK;
    }
    if (s_initializing) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_initializing = true;
    portEXIT_CRITICAL(&s_lock);

    esp_timer_handle_t timer = NULL;
    esp_err_t ret = device_state_init();
    if (ret == ESP_OK) {
        const esp_timer_create_args_t args = {
            .callback = timer_callback,
            .name = "controller_modes",
            .dispatch_method = ESP_TIMER_TASK,
        };
        ret = esp_timer_create(&args, &timer);
    }
    if (ret == ESP_OK) {
        ret = esp_timer_start_periodic(timer, 1000000);
        if (ret != ESP_OK) {
            /* This newly created timer did not start, so it has no live work. */
            esp_err_t delete_ret = esp_timer_delete(timer);
            if (delete_ret != ESP_OK) {
                ESP_LOGE(TAG, "Inactive timer cleanup failed: %s", esp_err_to_name(delete_ret));
            }
            timer = NULL;
        }
    }

    portENTER_CRITICAL(&s_lock);
    s_initializing = false;
    if (ret == ESP_OK) {
        s_boot_id = esp_random();
        s_timer = timer; /* Retained for the lifetime of app_main. */
        s_mode = APP_STATE_IDLE;
        s_duration_sec = 0;
        s_focus.remaining_us = 0; s_focus.active = false; s_focus.completed = false;
        s_completed = false;
        s_last_us = esp_timer_get_time();
        s_last_summary_us = s_last_us;
        s_initialized = true;
        mirror_locked(); /* Do not reset a chat state received before init. */
    }
    portEXIT_CRITICAL(&s_lock);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Mode controller ready: state/countdown only, timer=1 s, no hardware effects");
    } else {
        ESP_LOGW(TAG, "Mode controller init failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t controller_modes_focus_start(uint32_t minutes)
{
    if (minutes < 1 || minutes > 180) {
        return ESP_ERR_INVALID_ARG;
    }
    bool completed;
    uint32_t previous_duration;
    bool paused = false;
    esp_err_t ret = ESP_OK;
    portENTER_CRITICAL(&s_lock);
    if (!s_initialized) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    completed = advance_locked(esp_timer_get_time());
    previous_duration = s_duration_sec;
    if (s_mode == APP_STATE_FOCUS && !s_completed) {
        ret = ESP_ERR_INVALID_STATE; /* A repeated call must not erase progress. */
    } else {
        s_mode = APP_STATE_FOCUS;
        ++s_revision;
        s_duration_sec = minutes * 60;
        focus_clock_start(&s_focus, s_duration_sec, s_last_us);
        s_input_token = 0;
        s_completed = false;
        paused = s_voice_active;
        mirror_locked();
    }
    portEXIT_CRITICAL(&s_lock);
    log_completed(completed, previous_duration);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Focus started: duration=%u min paused=%d scope=state_only",
                 (unsigned)minutes, paused);
    }
    return ret;
}

esp_err_t controller_modes_companion_start(void)
{
    bool completed;
    bool changed;
    uint32_t previous_duration;
    portENTER_CRITICAL(&s_lock);
    if (!s_initialized) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    completed = advance_locked(esp_timer_get_time());
    previous_duration = s_duration_sec;
    changed = s_mode != APP_STATE_COMPANION;
    if (changed) {
        s_mode = APP_STATE_COMPANION;
        ++s_revision;
        s_duration_sec = 0;
        s_focus.remaining_us = 0; s_focus.active = false; s_focus.completed = false;
        s_completed = false;
        mirror_locked();
    }
    portEXIT_CRITICAL(&s_lock);
    log_completed(completed, previous_duration);
    if (changed) {
        ESP_LOGI(TAG, "Companion state set; demo worker applies lighting/head actions asynchronously");
    }
    return ESP_OK;
}

static esp_err_t stop_mode(app_state_t expected, bool any_mode)
{
    bool completed;
    bool changed;
    uint32_t previous_duration;
    app_state_t previous_mode;
    portENTER_CRITICAL(&s_lock);
    if (!s_initialized) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    completed = advance_locked(esp_timer_get_time());
    previous_duration = s_duration_sec;
    previous_mode = s_mode;
    changed = any_mode || s_mode == expected;
    if (changed) {
        if (s_mode != APP_STATE_IDLE || s_completed) ++s_revision;
        s_mode = APP_STATE_IDLE;
        s_duration_sec = 0;
        s_focus.remaining_us = 0; s_focus.active = false; s_focus.completed = false;
        s_completed = false;
        mirror_locked();
    }
    portEXIT_CRITICAL(&s_lock);
    log_completed(completed, previous_duration);
    if (changed) {
        ESP_LOGI(TAG, "Mode stopped: previous=%s mode=idle scope=state_only",
                 controller_modes_name(previous_mode));
    }
    return ESP_OK;
}

esp_err_t controller_modes_focus_stop(void)
{
    return stop_mode(APP_STATE_FOCUS, false);
}

esp_err_t controller_modes_companion_stop(void)
{
    return stop_mode(APP_STATE_COMPANION, false);
}

esp_err_t controller_modes_stop(void)
{
    return stop_mode(APP_STATE_IDLE, true);
}

void controller_modes_set_voice_active(bool active)
{
    bool completed;
    bool pause_changed;
    uint32_t duration;
    portENTER_CRITICAL(&s_lock);
    completed = advance_locked(esp_timer_get_time());
    duration = s_duration_sec;
    pause_changed = s_initialized && s_mode == APP_STATE_FOCUS && s_voice_active != active;
    s_voice_active = active;
    focus_clock_voice(&s_focus, active, esp_timer_get_time());
    /* The local value always survives init; an unavailable mirror is harmless. */
    if (s_initialized) {
        mirror_locked();
    }
    portEXIT_CRITICAL(&s_lock);
    log_completed(completed, duration);
    if (pause_changed) {
        ESP_LOGI(TAG, "Focus countdown %s: voice dialogue %s",
                 active ? "paused" : "resumed", active ? "active" : "ended");
    }
}

bool controller_modes_voice_active(void)
{
    portENTER_CRITICAL(&s_lock);
    bool active = s_voice_active;
    portEXIT_CRITICAL(&s_lock);
    return active;
}

esp_err_t controller_modes_get_status(controller_modes_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    if (!s_initialized) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    bool completed = advance_locked(esp_timer_get_time());
    snapshot_locked(out);
    portEXIT_CRITICAL(&s_lock);
    log_completed(completed, out->duration_sec);
    return ESP_OK;
}

static int engagement_rate(const char *level)
{
    if (!strcmp(level, "HighlyEngaged") || !strcmp(level, "Engaged") || !strcmp(level,"高度参与") || !strcmp(level,"参与")) return 2;
    if (!strcmp(level, "BarelyEngaged") || !strcmp(level,"勉强参与")) return 1;
    if (!strcmp(level, "NotEngaged") || !strcmp(level,"未参与")) return 0;
    return -1;
}
esp_err_t controller_modes_engagement(uint32_t boot, uint32_t session, uint32_t token,
    bool bind, uint32_t sample, uint32_t inference, uint32_t age_ms, uint32_t sample_age_ms,
    bool valid, const char *level)
{
    int rate = engagement_rate(level ? level : "");
    portENTER_CRITICAL(&s_lock);
    int64_t now = esp_timer_get_time();
    advance_locked(now);
    if (!s_initialized || boot != s_boot_id || session != s_focus.session ||
        s_mode != APP_STATE_FOCUS || s_completed || (!bind && (!token || token != s_input_token))) {
        portEXIT_CRITICAL(&s_lock); return ESP_ERR_INVALID_STATE;
    }
    if (bind) {
        focus_clock_bind(&s_focus, now);
        s_input_token = esp_random(); if (!s_input_token) s_input_token = 1;
    } else {
        focus_clock_sample(&s_focus, sample, inference, (int64_t)age_ms*1000,
                           (int64_t)sample_age_ms*1000, valid, rate, now);
    }
    controller_modes_status_t status; snapshot_locked(&status);
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "[FOCUS] session=%lu seq=%lu remaining_ms=%lu rate=%d/2 reason=%s age_ms=%lu",
        (unsigned long)status.session_id, (unsigned long)status.seq, (unsigned long)status.remaining_ms,
        status.rate_half, controller_modes_pause_name(status.pause_reason), (unsigned long)age_ms);
    return ESP_OK;
}
