/* Host fixture: execute the exact cache policy header used by the sampler. */
#include <stddef.h>
#include "../components/motion_controller/include/motion_position_cache.h"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

/* The Windows host test links without a CRT or Windows SDK. */
#ifdef MOTION_POSITION_TEST_FREESTANDING
void *memcpy(void *destination, const void *source, size_t len)
{
    unsigned char *out = destination;
    const unsigned char *in = source;
    for (size_t i = 0; i < len; ++i) out[i] = in[i];
    return destination;
}

void *memset(void *destination, int value, size_t len)
{
    unsigned char *out = destination;
    for (size_t i = 0; i < len; ++i) out[i] = (unsigned char)value;
    return destination;
}
#endif

int motion_position_cache_test(void)
{
    motion_position_cache_entry_t cache[MOTION_POSITION_AXIS_COUNT] = {0};
    motion_axis_position_snapshot_t sample;
    const uint32_t ttl = MOTION_POSITION_STALE_AFTER_MS;

    CHECK(MOTION_POSITION_AXIS_COUNT == 5);
    CHECK(ttl == 10000);
    /* A zero-initialized raw field is not evidence of an observed zero. */
    for (uint8_t axis = 0; axis < MOTION_POSITION_AXIS_COUNT; ++axis) {
        sample = motion_position_cache_project(&cache[axis], axis, axis + 1, 0, ttl);
        CHECK(sample.axis == axis && sample.bus_id == axis + 1);
        CHECK(!sample.has_value && !sample.attempted && !sample.valid && sample.stale);
        CHECK(sample.raw == 0 && sample.age_ms == -1);
        CHECK(sample.last_attempt_at_ms == 0 && sample.last_ok_at_ms == 0);
        CHECK(sample.attempts == 0 && sample.successes == 0 && sample.failures == 0);
    }

    /* A first failed reply records an attempt, never a successful timestamp. */
    motion_position_cache_record(&cache[1], -1, 1000, 10);
    sample = motion_position_cache_project(&cache[1], 1, 2, 10, ttl);
    CHECK(!sample.has_value && sample.attempted && !sample.valid && sample.stale);
    CHECK(sample.last_attempt_at_ms == 10 && sample.last_ok_at_ms == 0);
    CHECK(sample.age_ms == -1);
    CHECK(sample.attempts == 1 && sample.successes == 0 && sample.failures == 1);

    /* Both protocols can report a real zero, including a boot-time timestamp. */
    motion_position_cache_record(&cache[0], 0, 3000, 0);
    sample = motion_position_cache_project(&cache[0], 0, 1, 0, ttl);
    CHECK(sample.has_value && sample.attempted && sample.valid && !sample.stale);
    CHECK(sample.raw == 0 && sample.last_ok_at_ms == 0 && sample.age_ms == 0);
    CHECK(sample.attempts == 1 && sample.successes == 1 && sample.failures == 0);
    motion_position_cache_record(&cache[1], 0, 1000, 100);
    sample = motion_position_cache_project(&cache[1], 1, 2, 100, ttl);
    CHECK(sample.has_value && sample.valid && !sample.stale && sample.raw == 0);
    CHECK(sample.last_ok_at_ms == 100);
    CHECK(sample.attempts == 2 && sample.successes == 1 && sample.failures == 1);

    /* Failure invalidates the latest read but retains the previous observation. */
    motion_position_cache_record(&cache[0], 2345, 3000, 100);
    motion_position_cache_record(&cache[2], 333, 1000, 150);
    motion_position_cache_record(&cache[0], -1, 3000, 200);
    sample = motion_position_cache_project(&cache[0], 0, 1, 200, ttl);
    CHECK(sample.has_value && sample.attempted && !sample.valid && !sample.stale);
    CHECK(sample.raw == 2345 && sample.last_ok_at_ms == 100 && sample.age_ms == 100);
    CHECK(sample.last_attempt_at_ms == 200);
    CHECK(sample.attempts == 3 && sample.successes == 2 && sample.failures == 1);
    sample = motion_position_cache_project(&cache[2], 2, 3, 200, ttl);
    CHECK(sample.has_value && sample.valid && !sample.stale);
    CHECK(sample.raw == 333 && sample.last_ok_at_ms == 150 && sample.age_ms == 50);
    CHECK(sample.last_attempt_at_ms == 150);
    CHECK(sample.attempts == 1 && sample.successes == 1 && sample.failures == 0);

    /* Freshness includes exactly the TTL, and projections cannot refresh time. */
    sample = motion_position_cache_project(&cache[2], 2, 3, 150 + ttl, ttl);
    CHECK(sample.valid && !sample.stale && sample.age_ms == ttl);
    sample = motion_position_cache_project(&cache[2], 2, 3, 151 + ttl, ttl);
    CHECK(!sample.valid && sample.stale && sample.age_ms == ttl + 1);
    CHECK(cache[2].last_ok_at_ms == 150 && cache[2].last_attempt_at_ms == 150);
    CHECK(cache[2].attempts == 1 && cache[2].successes == 1 && cache[2].failures == 0);
    sample = motion_position_cache_project(&cache[2], 2, 3, 149, ttl);
    CHECK(sample.has_value && !sample.valid && sample.stale && sample.age_ms == -1);
    CHECK(sample.raw == 333 && sample.last_ok_at_ms == 150);

    /* A new successful read recovers immediately from failed/stale observations. */
    motion_position_cache_record(&cache[0], 3000, 3000, 20000);
    sample = motion_position_cache_project(&cache[0], 0, 1, 20000, ttl);
    CHECK(sample.has_value && sample.valid && !sample.stale && sample.age_ms == 0);
    CHECK(sample.raw == 3000 && sample.last_ok_at_ms == 20000);
    CHECK(sample.last_attempt_at_ms == 20000);
    CHECK(sample.attempts == 4 && sample.successes == 3 && sample.failures == 1);
    motion_position_cache_record(&cache[2], 1000, 1000, 20000);
    sample = motion_position_cache_project(&cache[2], 2, 3, 20000, ttl);
    CHECK(sample.valid && !sample.stale && sample.raw == 1000 && sample.age_ms == 0);

    /* Protocol overflow and negative/error sentinels never become observations. */
    const int16_t maxima[] = {3000, 1000};
    const int16_t bad_values[] = {-1, -5, INT16_MIN, INT16_MAX};
    for (size_t protocol = 0; protocol < sizeof(maxima) / sizeof(maxima[0]); ++protocol) {
        motion_position_cache_entry_t rejected = {0};
        for (size_t bad = 0; bad < sizeof(bad_values) / sizeof(bad_values[0]); ++bad) {
            motion_position_cache_record(&rejected, bad_values[bad], maxima[protocol], 1 + bad);
            sample = motion_position_cache_project(&rejected, 3, 4, 20, ttl);
            CHECK(!sample.has_value && sample.attempted && !sample.valid && sample.stale);
            CHECK(sample.last_ok_at_ms == 0 && sample.age_ms == -1);
            CHECK(sample.successes == 0 && sample.failures == bad + 1);
        }
        motion_position_cache_record(&rejected, maxima[protocol] + 1, maxima[protocol], 30);
        sample = motion_position_cache_project(&rejected, 3, 4, 30, ttl);
        CHECK(!sample.has_value && !sample.valid && sample.last_ok_at_ms == 0);
        CHECK(sample.attempts == 5 && sample.successes == 0 && sample.failures == 5);
    }

    /* Saturated counters do not wrap, while values/timestamps still advance. */
    cache[4].attempts = UINT32_MAX - 1;
    cache[4].successes = UINT32_MAX - 1;
    cache[4].failures = UINT32_MAX - 1;
    motion_position_cache_record(&cache[4], 1, 1000, 40);
    CHECK(cache[4].attempts == UINT32_MAX && cache[4].successes == UINT32_MAX);
    CHECK(cache[4].failures == UINT32_MAX - 1);
    motion_position_cache_record(&cache[4], INT16_MIN, 1000, 41);
    sample = motion_position_cache_project(&cache[4], 4, 5, 41, ttl);
    CHECK(sample.attempted && sample.has_value && !sample.valid);
    CHECK(sample.raw == 1 && sample.last_ok_at_ms == 40 && sample.last_attempt_at_ms == 41);
    CHECK(sample.attempts == UINT32_MAX && sample.successes == UINT32_MAX);
    CHECK(sample.failures == UINT32_MAX);
    motion_position_cache_record(&cache[4], 2, 1000, 42);
    motion_position_cache_record(&cache[4], -1, 1000, 43);
    motion_position_cache_record(&cache[4], 3, 1000, 44);
    sample = motion_position_cache_project(&cache[4], 4, 5, 44, ttl);
    CHECK(sample.attempted && sample.valid && !sample.stale && sample.raw == 3);
    CHECK(sample.last_ok_at_ms == 44 && sample.last_attempt_at_ms == 44);
    CHECK(sample.attempts == UINT32_MAX && sample.successes == UINT32_MAX);
    CHECK(sample.failures == UINT32_MAX);
    return 0;
}
