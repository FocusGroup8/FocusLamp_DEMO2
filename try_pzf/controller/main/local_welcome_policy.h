#ifndef LOCAL_WELCOME_POLICY_H
#define LOCAL_WELCOME_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#define LOCAL_WELCOME_COUNT 9

/* Zero initialization starts at item 1 (index 0). Callers serialize every
 * access, including cancellation checks and the actual playback start. */
typedef struct {
    uint8_t next_index;
    uint8_t active_index;
    bool pending;
    bool active;
    bool started;
    uint32_t generation;
    uint32_t active_generation;
} local_welcome_policy_t;

/* Any burst of requests retains at most one pending playback. */
static inline void local_welcome_policy_request(local_welcome_policy_t *s)
{
    if (s) {
        s->pending = true;
    }
}

static inline bool local_welcome_policy_take(local_welcome_policy_t *s,
                                             uint32_t *generation_out,
                                             uint8_t *index_out)
{
    if (!s || s->active || !s->pending) {
        return false;
    }
    ++s->generation;
    s->active_generation = s->generation;
    s->active_index = s->next_index;
    s->active = true;
    s->pending = false;
    s->started = false;
    if (generation_out) {
        *generation_out = s->active_generation;
    }
    if (index_out) {
        *index_out = s->active_index;
    }
    return true;
}

static inline bool local_welcome_policy_cancelled(const local_welcome_policy_t *s,
                                                  uint32_t generation)
{
    return !s || !s->active || s->active_generation != generation ||
           s->generation != generation;
}

/* Commit a report that PCM actually reached the output. Cancellation can race
 * with that output, so only slot ownership matters here, not generation. */
static inline bool local_welcome_policy_output_started(local_welcome_policy_t *s,
                                                       uint32_t generation)
{
    if (!s || !s->active || s->active_generation != generation || s->started) {
        return false;
    }
    s->started = true;
    s->next_index = (uint8_t)((s->active_index + 1U) % LOCAL_WELCOME_COUNT);
    return true;
}

/* For callers that can validate and commit actual output in one serialized
 * step. Blocking playback callers report output_started after it returns. */
static inline bool local_welcome_policy_start(local_welcome_policy_t *s,
                                              uint32_t generation)
{
    if (local_welcome_policy_cancelled(s, generation)) {
        return false;
    }
    return local_welcome_policy_output_started(s, generation);
}

/* Keep the active slot owned by its worker until finish; this prevents a
 * replacement worker from overlapping cleanup of the cancelled playback. */
static inline void local_welcome_policy_cancel(local_welcome_policy_t *s)
{
    if (s) {
        ++s->generation;
        s->pending = false;
    }
}

/* A cancelled worker can finish after generation changes. A newer request
 * remains pending, and a stale finish cannot release a newer active slot. */
static inline void local_welcome_policy_finish(local_welcome_policy_t *s,
                                               uint32_t generation)
{
    if (!s || s->active_generation != generation) {
        return;
    }
    s->active = false;
    s->started = false;
}

#endif /* LOCAL_WELCOME_POLICY_H */
