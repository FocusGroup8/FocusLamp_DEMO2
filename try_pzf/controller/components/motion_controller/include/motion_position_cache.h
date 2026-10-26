#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MOTION_POSITION_AXIS_COUNT 5
#define MOTION_POSITION_STALE_AFTER_MS 10000U

/* Readback is not a target, a reference pose, or a mechanical safety limit. */
typedef struct {
    uint8_t axis, bus_id;
    bool has_value, attempted, valid, stale;
    int16_t raw;
    int64_t last_attempt_at_ms, last_ok_at_ms, age_ms;
    uint32_t attempts, successes, failures;
} motion_axis_position_snapshot_t;

typedef struct {
    bool sampling_started, motion_busy;
    int64_t captured_at_ms;
    uint32_t stale_after_ms;
    motion_axis_position_snapshot_t axes[MOTION_POSITION_AXIS_COUNT];
} motion_position_snapshot_t;

/* The owner supplies synchronization. Zero initialization means no observation. */
typedef struct {
    bool has_value, last_read_ok;
    int16_t raw;
    int64_t last_attempt_at_ms, last_ok_at_ms;
    uint32_t attempts, successes, failures;
} motion_position_cache_entry_t;

static inline void motion_position_count(uint32_t *counter)
{
    if (*counter < UINT32_MAX) ++*counter;
}

/* Protocol bounds only validate a reply; they never authorize movement. */
static inline void motion_position_cache_record(motion_position_cache_entry_t *entry,
                                                int16_t raw, int16_t protocol_max,
                                                int64_t completed_at_ms)
{
    motion_position_count(&entry->attempts);
    entry->last_attempt_at_ms=completed_at_ms;
    entry->last_read_ok=raw>=0 && raw<=protocol_max;
    if (entry->last_read_ok) {
        entry->has_value=true;
        entry->raw=raw;
        entry->last_ok_at_ms=completed_at_ms;
        motion_position_count(&entry->successes);
    } else {
        /* Preserve the last good raw/time, without passing it off as a new read. */
        motion_position_count(&entry->failures);
    }
}

static inline motion_axis_position_snapshot_t motion_position_cache_project(
    const motion_position_cache_entry_t *entry, uint8_t axis, uint8_t bus_id,
    int64_t now_ms, uint32_t stale_after_ms)
{
    int64_t age=entry->has_value && now_ms>=entry->last_ok_at_ms ?
        now_ms-entry->last_ok_at_ms : -1;
    bool stale=age<0 || age>(int64_t)stale_after_ms;
    return (motion_axis_position_snapshot_t){
        .axis=axis, .bus_id=bus_id,
        .has_value=entry->has_value, .attempted=entry->attempts!=0,
        .valid=entry->last_read_ok && !stale, .stale=stale,
        .raw=entry->raw, .last_attempt_at_ms=entry->last_attempt_at_ms,
        .last_ok_at_ms=entry->last_ok_at_ms, .age_ms=age,
        .attempts=entry->attempts, .successes=entry->successes,
        .failures=entry->failures,
    };
}
