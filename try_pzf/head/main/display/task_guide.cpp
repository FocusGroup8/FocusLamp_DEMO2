// Head-side glue for the three-step task guide:
//   controller snapshot -> Model (local clock) -> report back to controller
//   GT911 press/release (via the LVGL canvas) -> Model::click
//   Model view -> task_guide::Screen on the shared eye canvas
// Behaviour is specified in controller/docs/task-guide-spec.md.

#include "task_guide.h"

#include "task_guide_model.h"
#include "task_guide_view.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

constexpr const char *TAG = "TASK_GUIDE";
constexpr int64_t kNoticeMs = 4000;         // "暂时没收到结果" after a controller timeout
constexpr int64_t kRunningReportMs = 5000;  // progress report period while running

// Everything below is shared between the HTTP sync task, the LVGL input
// callback and the animation task, and is only touched under `guard`.
portMUX_TYPE guard = portMUX_INITIALIZER_UNLOCKED;
task_guide::Model model;
bool touch_available = false;

// A click is one press and one release on the same request.
bool pressed = false;
uint32_t pressed_boot = 0, pressed_request = 0;

// Reports: a new sequence per model revision (and periodically while running);
// the controller acknowledges by sequence.
uint32_t report_sequence = 0, acked_sequence = 0, model_revision = 0;
int64_t last_report_ms = 0;

// Timeout notice, only for a waiting request this head has seen.
uint32_t waiting_boot = 0, waiting_request = 0;
int64_t notice_until_ms = 0;

// Drawing state, owned by the animation task (it holds the LVGL lock).
task_guide::Screen screen;
bool drew_last_frame = false;

int64_t now_ms() { return esp_timer_get_time() / 1000; }

// The normal, non-focus screen is mode=idle. Companion is an optional named mode,
// not a prerequisite for a user asking Xiaozhi to break a task into three steps.
bool guide_mode(const char *mode) { return !std::strcmp(mode, "idle") || !std::strcmp(mode, "companion"); }

struct Trace {
    uint32_t revision, remaining_ms;
    unsigned step;
    const char *state;
};

Trace trace_locked()
{
    const auto &v = model.view();
    return {v.revision, v.remaining_ms, v.step, model.report_state()};
}

void log_transition(const Trace &before, const Trace &after, const char *source)
{
    if (before.revision == after.revision) return;
#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "Head %s: state=%s step=%u remaining_ms=%lu", source, after.state, after.step + 1,
             (unsigned long)after.remaining_ms);
#else
    (void)source;
#endif
}

bool read_u32(const cJSON *object, const char *key, uint32_t &result)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(v) || !std::isfinite(v->valuedouble) || v->valuedouble < 0 ||
        v->valuedouble > UINT32_MAX || std::floor(v->valuedouble) != v->valuedouble)
        return false;
    result = static_cast<uint32_t>(v->valuedouble);
    return true;
}

/* Call under guard after every model update. */
void note_changes_locked(int64_t now)
{
    const auto &v = model.view();
    if (v.revision == model_revision) return;
    model_revision = v.revision;
    ++report_sequence;
    last_report_ms = now;
    // A press that started on another request (or before dismissal) is void.
    if (v.state == task_guide::State::Idle || v.controller_boot_id != pressed_boot || v.request_id != pressed_request)
        pressed = false;
}

} // namespace

extern "C" void task_guide_set_touch_available(bool available)
{
    portENTER_CRITICAL(&guard);
    touch_available = available;
    pressed = false;
    portEXIT_CRITICAL(&guard);
}

extern "C" void task_guide_observe_mode(const cJSON *snapshot)
{
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(snapshot, "mode");
    if (!cJSON_IsString(mode)) return;
    const cJSON *guide = cJSON_GetObjectItemCaseSensitive(snapshot, "task_guide");
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(guide, "state");
    uint32_t boot = 0, id = 0;
    if (!cJSON_IsString(state) || !read_u32(guide, "controller_boot_id", boot) || !read_u32(guide, "request_id", id))
        return;
    const char *steps[TG_STEP_COUNT] = {nullptr, nullptr, nullptr};
    if (!std::strcmp(state->valuestring, "ready")) {
        const cJSON *array = cJSON_GetObjectItemCaseSensitive(guide, "steps");
        if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) != TG_STEP_COUNT) return;
        for (int i = 0; i < TG_STEP_COUNT; i++) {
            const cJSON *item = cJSON_GetArrayItem(array, i);
            if (!cJSON_IsString(item)) return;
            steps[i] = item->valuestring;
        }
    }

    const int64_t now = now_ms();
    const bool allowed = guide_mode(mode->valuestring);
    portENTER_CRITICAL(&guard);
    const Trace before = trace_locked();
    if (!std::strcmp(state->valuestring, "waiting")) {
        waiting_boot = boot;
        waiting_request = id;
        notice_until_ms = 0;
    }
    if (!std::strcmp(state->valuestring, "timeout") && waiting_boot == boot && waiting_request == id) {
        notice_until_ms = now + kNoticeMs;
        waiting_boot = waiting_request = 0;
    }
    model.observe(boot, id, state->valuestring, steps, allowed, now);
    // A failed GT911 registration must not leave a preview that cannot be started/exited.
    const bool no_touch = !touch_available && model.view().state == task_guide::State::Preview;
    if (no_touch) model.cancel();
    if (!allowed) notice_until_ms = 0;
    note_changes_locked(now);
    const Trace after = trace_locked();
    portEXIT_CRITICAL(&guard);

    log_transition(before, after, "controller snapshot");
    if (no_touch) ESP_LOGW(TAG, "GT911 input unavailable; guide dismissed rather than showing an unusable preview");
}

extern "C" void task_guide_touch_pressed(void)
{
    portENTER_CRITICAL(&guard);
    const auto &v = model.view();
    pressed = touch_available && (v.state == task_guide::State::Preview || v.state == task_guide::State::Running);
    pressed_boot = v.controller_boot_id;
    pressed_request = v.request_id;
    portEXIT_CRITICAL(&guard);
}

extern "C" void task_guide_touch_cancelled(void)
{
    portENTER_CRITICAL(&guard);
    pressed = false;
    portEXIT_CRITICAL(&guard);
}

extern "C" void task_guide_touch_released(void)
{
    const int64_t now = now_ms();
    portENTER_CRITICAL(&guard);
    const Trace before = trace_locked();
    const auto &v = model.view();
    if (pressed && pressed_boot == v.controller_boot_id && pressed_request == v.request_id) model.click(now);
    pressed = false;
    note_changes_locked(now);
    const Trace after = trace_locked();
    portEXIT_CRITICAL(&guard);
    log_transition(before, after, "screen release");
}

extern "C" bool task_guide_report(char *json, size_t capacity, uint32_t *sequence)
{
    const int64_t now = now_ms();
    task_guide::View view;
    const char *state = nullptr;
    portENTER_CRITICAL(&guard);
    const Trace before = trace_locked();
    model.update(now);
    note_changes_locked(now);
    view = model.view();
    state = model.report_state();
    if (view.state == task_guide::State::Running && now - last_report_ms >= kRunningReportMs &&
        acked_sequence == report_sequence) {
        ++report_sequence;
        last_report_ms = now;
    }
    const bool pending = report_sequence != acked_sequence && view.request_id && state;
    *sequence = report_sequence;
    const Trace after = trace_locked();
    portEXIT_CRITICAL(&guard);
    log_transition(before, after, "local timer");
    if (!pending) return false;
    const int n = std::snprintf(json, capacity,
        "{\"controller_boot_id\":%lu,\"request_id\":%lu,\"state\":\"%s\",\"step\":%u,\"remaining_ms\":%lu,\"report_seq\":%lu}",
        (unsigned long)view.controller_boot_id, (unsigned long)view.request_id, state, view.step,
        (unsigned long)view.remaining_ms, (unsigned long)*sequence);
    return n > 0 && static_cast<size_t>(n) < capacity;
}

extern "C" void task_guide_report_ack(uint32_t sequence)
{
    portENTER_CRITICAL(&guard);
    if (sequence == report_sequence) acked_sequence = sequence;
    portEXIT_CRITICAL(&guard);
}

void task_guide_release_graphics()
{
    if (drew_last_frame) screen.release();
    drew_last_frame = false;
}

bool task_guide_draw(uint8_t *bytes, int width, int height, int stride, int64_t now)
{
    task_guide::Frame frame;
    portENTER_CRITICAL(&guard);
    const Trace before = trace_locked();
    model.update(now);
    note_changes_locked(now);
    frame.view = model.view();
    frame.notice = now < notice_until_ms;
    const Trace after = trace_locked();
    portEXIT_CRITICAL(&guard);
    log_transition(before, after, "local timer");

    if (frame.view.state == task_guide::State::Idle && !frame.notice) {
        task_guide_release_graphics();
        return false;
    }
    // Draw outside the spinlock; the frame is a private copy.
    screen.draw(fola::Canvas(bytes, width, height, stride), frame, now, drew_last_frame);
    drew_last_frame = true;
    return true;
}
