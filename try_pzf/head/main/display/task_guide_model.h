#pragma once

#include "../../../shared/task_guide_protocol.h"
#include <cstdint>
#include <cstring>

namespace task_guide {

enum class State { Idle, Preview, Running, Done };

struct View {
    State state = State::Idle;
    uint32_t controller_boot_id = 0;
    uint32_t request_id = 0;
    uint32_t revision = 0;
    unsigned step = 0;
    uint32_t remaining_ms = 0;
    char steps[TG_STEP_COUNT][TG_STEP_BYTES] = {};
};

/* Pure local authority: no networking, wall clock, or dynamic allocations.
 * observe() must be called only for valid, successful controller snapshots.
 * No call on transport failure is needed: the local clock simply continues. */
class Model {
public:
    static constexpr uint32_t step_ms = 180000;
    static constexpr uint32_t done_ms = 2000;

    const View &view() const { return view_; }
    const char *report_state() const { return report_state_; }
    void cancel()
    {
        if (view_.state == State::Preview || view_.state == State::Running) dismiss();
    }

    bool observe(uint32_t boot, uint32_t request, const char *request_state,
                 const char *const steps[TG_STEP_COUNT], bool companion, int64_t now_ms)
    {
        update(now_ms);
        if (!boot || !request_state || !known_state(request_state)) return false;
        const bool first = !observed_;
        if (seen_boot_ != boot) {
            seen_boot_ = boot;
            seen_request_ = handled_request_ = 0;
        }
        observed_ = true;
        if (request < seen_request_) return false;
        seen_request_ = request;

        /* A head restart must not replay a controller's old ready payload.
         * First waiting -> ready is valid; first ready establishes a dismissed
         * baseline and requires a genuinely newer user request. */
        if (first && !std::strcmp(request_state, "ready") && request) {
            handled_request_ = request;
            view_.controller_boot_id = boot;
            view_.request_id = request;
            dismiss();
            return false;
        }

        if (!companion) {
            if (request > handled_request_) handled_request_ = request;
            if (view_.state == State::Preview || view_.state == State::Running) dismiss();
            return false;
        }

        const bool same = view_.controller_boot_id == boot && view_.request_id == request;
        if (!std::strcmp(request_state, "dismissed")) {
            if (request > handled_request_) handled_request_ = request;
            if (same && (view_.state == State::Preview || view_.state == State::Running)) dismiss();
            return false;
        }
        /* A new waiting request does not interrupt a task already in progress.
         * Only validated new content replaces the current guide. */
        if (std::strcmp(request_state, "ready") || !request || request <= handled_request_)
            return false;
        if (!tg_validate_steps(steps)) return false;

        handled_request_ = request;
        view_.controller_boot_id = boot;
        view_.request_id = request;
        for (unsigned i = 0; i < TG_STEP_COUNT; ++i)
            std::memcpy(view_.steps[i], steps[i], std::strlen(steps[i]) + 1);
        view_.state = State::Preview;
        view_.step = 0;
        view_.remaining_ms = step_ms;
        report_state_ = "preview";
        changed();
        return true;
    }

    /* Invoke exactly once per completed GT911 press-release gesture.
     * Returns whether the guide consumed the release. A second real gesture
     * during running exits even when it follows the start gesture quickly. */
    bool click(int64_t now_ms)
    {
        update(now_ms);
        if (view_.state == State::Preview) {
            started_ms_ = now_ms;
            view_.state = State::Running;
            view_.step = 0;
            view_.remaining_ms = step_ms;
            report_state_ = "running";
            changed();
            return true;
        }
        if (view_.state == State::Running) {
            dismiss();
            return true;
        }
        return view_.state == State::Done;
    }

    void update(int64_t now_ms)
    {
        if (view_.state == State::Running) {
            const uint64_t elapsed = now_ms > started_ms_ ? uint64_t(now_ms - started_ms_) : 0;
            if (elapsed >= uint64_t(step_ms) * TG_STEP_COUNT) {
                view_.state = State::Done;
                view_.step = TG_STEP_COUNT - 1;
                view_.remaining_ms = 0;
                report_state_ = "done";
                done_at_ms_ = started_ms_ + int64_t(step_ms) * TG_STEP_COUNT;
                changed();
            } else {
                const unsigned step = unsigned(elapsed / step_ms);
                const uint32_t remaining = step_ms - uint32_t(elapsed % step_ms);
                if (step != view_.step) {
                    view_.step = step;
                    view_.remaining_ms = remaining;
                    changed();
                } else if (remaining < view_.remaining_ms) {
                    view_.remaining_ms = remaining;
                }
            }
        }
        if (view_.state == State::Done && now_ms - done_at_ms_ >= done_ms) {
            view_.state = State::Idle;
            /* Preserve the terminal report (and its revision) until the
             * transport can acknowledge it, including after a long outage. */
        }
    }

private:
    static bool known_state(const char *state)
    {
        return !std::strcmp(state, "idle") || !std::strcmp(state, "waiting") ||
            !std::strcmp(state, "ready") || !std::strcmp(state, "timeout") ||
            !std::strcmp(state, "dismissed") || !std::strcmp(state, "done");
    }
    void changed() { if (!++view_.revision) ++view_.revision; }
    void dismiss()
    {
        view_.state = State::Idle;
        view_.remaining_ms = 0;
        report_state_ = "dismissed";
        changed();
    }

    View view_;
    const char *report_state_ = "idle";
    bool observed_ = false;
    uint32_t seen_boot_ = 0, seen_request_ = 0, handled_request_ = 0;
    int64_t started_ms_ = 0, done_at_ms_ = 0;
};

} // namespace task_guide
