#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Echoed timestamps belong to the receiver's monotonic clock. The sender
 * reports only its locally measured residence after receiving that echo.
 * Removing that residence avoids charging the same wait again in link age.
 * What remains includes both network directions, so it is conservative.
 * Missing residence is supplied as zero by legacy callers. */
static inline bool focus_transport_age(uint32_t now_ms, uint32_t echo_ms,
                                       uint32_t hold_ms, uint32_t *age_ms)
{
    uint32_t elapsed = now_ms - echo_ms; /* also handles one uint32 wrap */
    if (!age_ms || hold_ms > elapsed) return false;
    uint32_t transit = elapsed - hold_ms;
    if (transit > 3000) return false;
    *age_ms = transit;
    return true;
}

static inline uint32_t focus_age_add(uint32_t age, uint32_t delta)
{
    return age > UINT32_MAX - delta ? UINT32_MAX : age + delta;
}
