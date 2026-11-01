#pragma once
#include <stdbool.h>
#include <stdint.h>

/* A failed HTTP exchange does not prove that a bind or sample was rejected.
 * Probe first, preserve the last identities, and never replay a sent sample. */
typedef struct {
    bool probe, pending_bind;
    uint32_t pending_generation, previous_token;
} focus_sync_recovery_t;

static inline void focus_sync_bind_started(focus_sync_recovery_t *state,
                                            uint32_t generation, uint32_t token)
{
    state->pending_bind = true;
    state->pending_generation = generation;
    state->previous_token = token;
}

static inline bool focus_sync_failed(focus_sync_recovery_t *state, int status)
{
    state->probe = true;
    if (status == 409) {
        state->pending_bind = false;
        return true; /* explicit boot/session/token conflict requires resync */
    }
    return false;
}

/* A timed-out bind may have succeeded. A changed token in the next authority
 * probe confirms it; an unchanged token cannot confirm a newly requested bind.
 * Generation matching prevents adopting an old-source bind after source change. */
static inline bool focus_sync_bind_confirmed(const focus_sync_recovery_t *state,
    bool direct_bind_reply, uint32_t generation, uint32_t token)
{
    return state->pending_bind && state->pending_generation == generation && token &&
        (direct_bind_reply || token != state->previous_token);
}
