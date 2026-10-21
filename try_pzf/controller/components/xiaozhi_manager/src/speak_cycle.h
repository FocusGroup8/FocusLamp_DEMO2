#ifndef XIAOZHI_SPEAK_CYCLE_H
#define XIAOZHI_SPEAK_CYCLE_H

#include <stdbool.h>
#include <stdint.h>

/* Callers serialize access. A generation protects local worker/timeout work;
 * it does not identify SDK events that carry no request/session identifier. */
typedef enum {
    SPEAK_IDLE = 0,
    SPEAK_OPENING,
    SPEAK_WAITING,
    SPEAK_PLAYING,
    SPEAK_CLOSING,
} speak_phase_t;

typedef enum {
    SPEAK_OUTCOME_NONE = 0,
    SPEAK_OUTCOME_DONE,
    SPEAK_OUTCOME_ERROR,
    SPEAK_OUTCOME_TIMEOUT,
    SPEAK_OUTCOME_CANCELLED,
    SPEAK_OUTCOME_DISCONNECTED,
} speak_outcome_t;

typedef struct {
    speak_phase_t phase;
    uint32_t generation;
    int64_t wait_started_us;
    speak_outcome_t outcome;
} speak_cycle_t;

static inline bool speak_cycle_busy(const speak_cycle_t *cycle)
{
    return cycle && cycle->phase != SPEAK_IDLE;
}

static inline bool speak_cycle_begin(speak_cycle_t *cycle, uint32_t *generation_out)
{
    if (!cycle || cycle->phase != SPEAK_IDLE) {
        return false;
    }
    ++cycle->generation;
    cycle->phase = SPEAK_OPENING;
    cycle->wait_started_us = 0;
    cycle->outcome = SPEAK_OUTCOME_NONE;
    if (generation_out) {
        *generation_out = cycle->generation;
    }
    return true;
}

/* Register WAITING before SDK send calls, so a rapid START cannot be lost. */
static inline bool speak_cycle_wait(speak_cycle_t *cycle, uint32_t generation,
                                    int64_t now_us)
{
    if (!cycle || cycle->generation != generation ||
        cycle->phase != SPEAK_OPENING || now_us < 0) {
        return false;
    }
    cycle->phase = SPEAK_WAITING;
    cycle->wait_started_us = now_us;
    return true;
}

static inline bool speak_cycle_start(speak_cycle_t *cycle, uint32_t generation)
{
    if (!cycle || cycle->generation != generation ||
        (cycle->phase != SPEAK_WAITING && cycle->phase != SPEAK_PLAYING)) {
        return false;
    }
    cycle->phase = SPEAK_PLAYING;
    return true;
}

static inline bool speak_cycle_finish(speak_cycle_t *cycle, uint32_t generation,
                                      speak_outcome_t outcome)
{
    if (!cycle || cycle->generation != generation ||
        cycle->phase == SPEAK_IDLE || cycle->phase == SPEAK_CLOSING ||
        outcome == SPEAK_OUTCOME_NONE ||
        (outcome == SPEAK_OUTCOME_DONE && cycle->phase != SPEAK_PLAYING)) {
        return false;
    }
    cycle->phase = SPEAK_CLOSING;
    cycle->outcome = outcome;
    return true;
}

/* A first-response timeout must never truncate an already-playing utterance. */
static inline bool speak_cycle_expire(speak_cycle_t *cycle, uint32_t generation,
                                      int64_t now_us, int64_t timeout_us)
{
    if (!cycle || cycle->generation != generation ||
        cycle->phase != SPEAK_WAITING || timeout_us <= 0 ||
        now_us < cycle->wait_started_us ||
        now_us - cycle->wait_started_us < timeout_us) {
        return false;
    }
    return speak_cycle_finish(cycle, generation, SPEAK_OUTCOME_TIMEOUT);
}

/* Release only after the owning worker has completed transport cleanup. */
static inline bool speak_cycle_release(speak_cycle_t *cycle, uint32_t generation)
{
    if (!cycle || cycle->generation != generation || cycle->phase != SPEAK_CLOSING) {
        return false;
    }
    cycle->phase = SPEAK_IDLE;
    cycle->wait_started_us = 0;
    /* Preserve the completed outcome until the next begin for diagnostics. */
    return true;
}

#endif /* XIAOZHI_SPEAK_CYCLE_H */
