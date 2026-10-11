#include "controller_modes.h"

#include <stddef.h>

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
static int64_t s_elapsed_us;
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
    int64_t delta_us = now_us - s_last_us;
    s_last_us = now_us;
    if (!s_initialized || s_mode != APP_STATE_FOCUS || s_voice_active || delta_us <= 0) {
        return false;
    }

    int64_t duration_us = (int64_t)s_duration_sec * 1000000;
    int64_t remaining_us = duration_us - s_elapsed_us;
    if (delta_us >= remaining_us) {
        s_elapsed_us = duration_us;
        s_mode = APP_STATE_IDLE;
        ++s_revision;
        s_completed = true;
        mirror_locked();
        return true;
    }
    s_elapsed_us += delta_us;
    return false;
}

static void snapshot_locked(controller_modes_status_t *out)
{
    int64_t remaining_us = (int64_t)s_duration_sec * 1000000 - s_elapsed_us;
    *out = (controller_modes_status_t) {
        .mode = s_mode,
        .duration_sec = s_duration_sec,
        .remaining_sec = remaining_us > 0 ? (uint32_t)((remaining_us + 999999) / 1000000) : 0,
        .voice_active = s_voice_active,
        .paused = s_mode == APP_STATE_FOCUS && s_voice_active,
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
        s_timer = timer; /* Retained for the lifetime of app_main. */
        s_mode = APP_STATE_IDLE;
        s_duration_sec = 0;
        s_elapsed_us = 0;
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
    if (s_mode == APP_STATE_FOCUS) {
        ret = ESP_ERR_INVALID_STATE; /* A repeated call must not erase progress. */
    } else {
        s_mode = APP_STATE_FOCUS;
        ++s_revision;
        s_duration_sec = minutes * 60;
        s_elapsed_us = 0;
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
        s_elapsed_us = 0;
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
        s_elapsed_us = 0;
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
