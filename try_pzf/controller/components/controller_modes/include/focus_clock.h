#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Pure monotonic-time engine; callers serialize access. All times are local us.
 * The renderer never advances this clock. 15 s accommodates the existing
 * classifier's 30-frame stride. The sampled demo explicitly allows 6 s of
 * face/sample age; normal builds retain 3 s. Transport validation is separate. */
#define FOCUS_CLASS_TTL_US 15000000LL
#if defined(FOCUS_SAMPLED_DEMO) && FOCUS_SAMPLED_DEMO
#define FOCUS_SAMPLE_TTL_US 6000000LL
#else
#define FOCUS_SAMPLE_TTL_US 3000000LL
#endif
#define FOCUS_DEBOUNCE_US 1000000LL
/* FocusCode dev-server-refactoring PomodoroManager: highly_focus=1, focus=.75,
 * distracted/tired=0. Keep this device's monotonic clock and freshness guards.
 * Commit b13ddfdfaade4054ce76b28dcc972c1b72a8610e. */
enum { FOCUS_WAIT, FOCUS_RUN, FOCUS_DISTRACTED, FOCUS_INVALID, FOCUS_STALE, FOCUS_VOICE, FOCUS_DONE };
typedef struct {
    int64_t remaining_us, last_us, started_us, expires_us, candidate_us, class_expires_us, sample_expires_us;
    uint32_t session, seq, sample_seq, inference_seq;
    int rate_quarters, candidate_quarters, reason;
    unsigned debit_remainder; /* Fractional microseconds, denominator 4. */
    bool active, completed, voice, has_sample;
    int64_t full_us, normal_us, held_us[FOCUS_DONE];
} focus_clock_t;

/* Wall-time residence, not effective debit. Completed/idle time is excluded. */
static inline void focus_clock_account(focus_clock_t *c, int64_t now)
{
    if (!c->active || c->completed || now <= c->last_us) return;
    int64_t elapsed = now - c->last_us;
    if (c->voice) { c->held_us[FOCUS_VOICE] += elapsed; return; }
    int64_t end = now < c->expires_us ? now : c->expires_us;
    int64_t fresh = end > c->last_us ? end - c->last_us : 0;
    if (c->rate_quarters > 0) {
        int64_t until_done = (c->remaining_us * 4 - c->debit_remainder + c->rate_quarters - 1) / c->rate_quarters;
        if (fresh > until_done) fresh = until_done;
        if (c->rate_quarters == 4) c->full_us += fresh;
        else c->normal_us += fresh;
        if (fresh == until_done) return;
    } else {
        unsigned reason = c->reason < FOCUS_DONE ? (unsigned)c->reason : FOCUS_WAIT;
        c->held_us[reason] += fresh;
    }
    if (elapsed > fresh) c->held_us[c->has_sample ? FOCUS_STALE : FOCUS_WAIT] += elapsed - fresh;
}

static inline void focus_clock_settle(focus_clock_t *c, int64_t now)
{
    focus_clock_account(c, now);
    int64_t end = now < c->expires_us ? now : c->expires_us;
    int64_t dt = end > c->last_us ? end - c->last_us : 0;
    if (c->active && !c->completed && !c->voice) {
        int64_t weighted = dt * c->rate_quarters + c->debit_remainder;
        int64_t debit = weighted / 4;
        c->debit_remainder = (unsigned)(weighted % 4);
        c->remaining_us = debit >= c->remaining_us ? 0 : c->remaining_us - debit;
        if (!c->remaining_us) { c->completed = true; c->debit_remainder = 0; c->rate_quarters = 0; c->reason = FOCUS_DONE; }
    }
    if (c->active && !c->completed && c->has_sample && now >= c->expires_us) {
        c->rate_quarters = 0; c->reason = FOCUS_STALE;
        /* Slow classifications may expire between two matching NEW results.
         * Keep only their debounce candidate while fresh valid face samples
         * continue. This never permits counting against an expired class. */
        if (now >= c->sample_expires_us) c->candidate_quarters = -1;
    }
    c->last_us = now;
}
static inline void focus_clock_start(focus_clock_t *c, uint32_t seconds, int64_t now)
{
    uint32_t session = c->session + 1;
    bool voice = c->voice;
    *c = (focus_clock_t){.remaining_us = (int64_t)seconds * 1000000,
        .last_us = now, .started_us = now, .session = session, .active = true,
        .voice = voice, .candidate_quarters = -1, .reason = FOCUS_WAIT};
}
static inline void focus_clock_bind(focus_clock_t *c, int64_t now)
{
    focus_clock_settle(c, now);
    c->rate_quarters = 0; c->reason = FOCUS_WAIT; c->candidate_quarters = -1;
    c->has_sample = false; c->sample_seq = c->inference_seq = 0;
    c->expires_us = c->sample_expires_us = c->class_expires_us = now;
}
static inline void focus_clock_voice(focus_clock_t *c, bool active, int64_t now)
{
    focus_clock_settle(c, now); c->voice = active;
}
/* sample_seq advances per analyzed frame; inference_seq only on a NEW model
 * classification. New face-invalid samples pause immediately. Cached valid
 * classifications can update face freshness but cannot extend class expiry
 * or satisfy debounce. age includes inference + all locally measured waits. */
static inline bool focus_clock_sample(focus_clock_t *c, uint32_t sample,
    uint32_t inference, int64_t class_age, int64_t sample_age, bool valid,
    int quarter_rate, int64_t now)
{
    focus_clock_settle(c, now);
    if (!c->active || c->completed || (c->has_sample && sample <= c->sample_seq)) return false;
    bool new_class = inference > c->inference_seq;
    c->has_sample = true; c->sample_seq = sample;
    if (!valid || (quarter_rate != 0 && quarter_rate != 3 && quarter_rate != 4)) {
        c->rate_quarters = 0; c->reason = FOCUS_INVALID; c->candidate_quarters = -1;
        c->inference_seq = inference; c->expires_us = now + FOCUS_SAMPLE_TTL_US;
        c->sample_expires_us = c->expires_us;
        return true;
    }
    if (sample_age < 0 || sample_age >= FOCUS_SAMPLE_TTL_US) {
        c->rate_quarters = 0; c->reason = FOCUS_STALE; c->candidate_quarters = -1;
        c->sample_expires_us = now;
        c->expires_us = now; return true;
    }
    c->sample_expires_us = now + FOCUS_SAMPLE_TTL_US - sample_age;
    /* Frames/classifications acquired before session entry cannot start time. */
    if (class_age < 0 || now - class_age < c->started_us) {
        c->rate_quarters = 0; c->reason = FOCUS_STALE; c->candidate_quarters = -1;
        c->expires_us = now; return true;
    }
    if (class_age >= FOCUS_CLASS_TTL_US) {
        c->rate_quarters = 0; c->reason = FOCUS_STALE;
        c->expires_us = now; return true;
    }
    int64_t class_end = now + FOCUS_CLASS_TTL_US - class_age;
    if (!new_class && c->class_expires_us && class_end > c->class_expires_us) class_end = c->class_expires_us;
    c->class_expires_us = class_end;
    int64_t sample_end = c->sample_expires_us;
    c->expires_us = class_end < sample_end ? class_end : sample_end;
    if (!new_class) return true;
    c->inference_seq = inference;
    if (c->candidate_quarters != quarter_rate) {
        c->candidate_quarters = quarter_rate; c->candidate_us = now;
    } else if (now - c->candidate_us >= FOCUS_DEBOUNCE_US) {
        c->rate_quarters = quarter_rate; c->reason = quarter_rate ? FOCUS_RUN : FOCUS_DISTRACTED;
    }
    return true;
}
