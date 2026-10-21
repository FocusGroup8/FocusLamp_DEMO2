/* This fixture exercises the actual policy header used by xiaozhi_manager. */
#include "../components/xiaozhi_manager/src/speak_cycle.h"
#include <stddef.h>

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

/* Windows Clang tests link without a CRT or Windows SDK. */
#ifdef SPEAK_TEST_FREESTANDING
void *memset(void *destination, int value, size_t len)
{
    unsigned char *out = destination;
    for (size_t i = 0; i < len; ++i) {
        out[i] = (unsigned char)value;
    }
    return destination;
}

void *memcpy(void *destination, const void *source, size_t len)
{
    unsigned char *out = destination;
    const unsigned char *in = source;
    for (size_t i = 0; i < len; ++i) {
        out[i] = in[i];
    }
    return destination;
}
#endif

int speak_cycle_test(void)
{
    speak_cycle_t cycle = {0};
    uint32_t first = 0;
    uint32_t second = 0;
    uint32_t unchanged = 99;

    CHECK(!speak_cycle_busy(&cycle));
    CHECK(!speak_cycle_busy(0));
    CHECK(!speak_cycle_begin(0, &first));
    CHECK(!speak_cycle_wait(0, 0, 0));
    CHECK(!speak_cycle_start(0, 0));
    CHECK(!speak_cycle_finish(0, 0, SPEAK_OUTCOME_ERROR));
    CHECK(!speak_cycle_expire(0, 0, 100, 50));
    CHECK(!speak_cycle_release(0, 0));

    CHECK(speak_cycle_begin(&cycle, &first));
    CHECK(first == 1 && cycle.phase == SPEAK_OPENING);
    CHECK(speak_cycle_busy(&cycle) && cycle.outcome == SPEAK_OUTCOME_NONE);
    CHECK(!speak_cycle_begin(&cycle, &unchanged) && unchanged == 99);
    CHECK(!speak_cycle_release(&cycle, first));
    CHECK(!speak_cycle_start(&cycle, first));
    CHECK(!speak_cycle_finish(&cycle, first, SPEAK_OUTCOME_DONE));
    CHECK(!speak_cycle_finish(&cycle, first, SPEAK_OUTCOME_NONE));
    CHECK(!speak_cycle_wait(&cycle, first, -1));
    CHECK(speak_cycle_wait(&cycle, first, 1000000));
    CHECK(cycle.wait_started_us == 1000000 && cycle.phase == SPEAK_WAITING);
    CHECK(!speak_cycle_finish(&cycle, first, SPEAK_OUTCOME_DONE));

    /* Register before SDK sends. A START arriving during them is preserved. */
    CHECK(speak_cycle_start(&cycle, first));
    CHECK(cycle.phase == SPEAK_PLAYING);
    CHECK(!speak_cycle_wait(&cycle, first, 1100000));
    CHECK(cycle.phase == SPEAK_PLAYING && cycle.wait_started_us == 1000000);
    CHECK(speak_cycle_start(&cycle, first));
    CHECK(!speak_cycle_expire(&cycle, first, 90000000, 5000000));
    CHECK(cycle.phase == SPEAK_PLAYING);

    CHECK(speak_cycle_finish(&cycle, first, SPEAK_OUTCOME_DONE));
    CHECK(cycle.phase == SPEAK_CLOSING && cycle.outcome == SPEAK_OUTCOME_DONE);
    CHECK(!speak_cycle_finish(&cycle, first, SPEAK_OUTCOME_ERROR));
    CHECK(!speak_cycle_expire(&cycle, first, 90000000, 5000000));
    CHECK(cycle.outcome == SPEAK_OUTCOME_DONE);
    CHECK(!speak_cycle_begin(&cycle, &second));
    CHECK(speak_cycle_release(&cycle, first));
    CHECK(!speak_cycle_busy(&cycle) && cycle.outcome == SPEAK_OUTCOME_DONE);
    CHECK(!speak_cycle_release(&cycle, first));

    /* Old worker/timer operations cannot mutate a replacement cycle. */
    CHECK(speak_cycle_begin(&cycle, &second));
    CHECK(second == first + 1 && cycle.outcome == SPEAK_OUTCOME_NONE);
    CHECK(!speak_cycle_wait(&cycle, first, 0));
    CHECK(!speak_cycle_start(&cycle, first));
    CHECK(!speak_cycle_finish(&cycle, first, SPEAK_OUTCOME_ERROR));
    CHECK(!speak_cycle_release(&cycle, first));
    CHECK(speak_cycle_wait(&cycle, second, 2000000));
    CHECK(!speak_cycle_expire(&cycle, first, 90000000, 5000000));
    CHECK(!speak_cycle_expire(&cycle, second, 1999999, 5000000));
    CHECK(!speak_cycle_expire(&cycle, second, 7000000, 0));
    CHECK(!speak_cycle_expire(&cycle, second, 6999999, 5000000));
    CHECK(speak_cycle_expire(&cycle, second, 7000000, 5000000));
    CHECK(cycle.phase == SPEAK_CLOSING && cycle.outcome == SPEAK_OUTCOME_TIMEOUT);
    CHECK(!speak_cycle_start(&cycle, second));
    CHECK(!speak_cycle_finish(&cycle, second, SPEAK_OUTCOME_DONE));
    CHECK(speak_cycle_release(&cycle, second));

    /* Non-success endings are legal before any START, and first result wins. */
    const speak_outcome_t endings[] = {
        SPEAK_OUTCOME_ERROR, SPEAK_OUTCOME_CANCELLED, SPEAK_OUTCOME_DISCONNECTED,
    };
    for (unsigned i = 0; i < sizeof(endings) / sizeof(endings[0]); ++i) {
        uint32_t generation;
        CHECK(speak_cycle_begin(&cycle, &generation));
        CHECK(speak_cycle_finish(&cycle, generation, endings[i]));
        CHECK(!speak_cycle_finish(&cycle, generation, SPEAK_OUTCOME_ERROR));
        CHECK(cycle.outcome == endings[i]);
        CHECK(speak_cycle_release(&cycle, generation));
        CHECK(!speak_cycle_busy(&cycle));
    }

    /* A completed failure does not prevent a normal subsequent utterance. */
    CHECK(speak_cycle_begin(&cycle, &second));
    CHECK(speak_cycle_wait(&cycle, second, 0));
    CHECK(speak_cycle_start(&cycle, second));
    CHECK(speak_cycle_finish(&cycle, second, SPEAK_OUTCOME_DONE));
    CHECK(speak_cycle_release(&cycle, second));
    CHECK(cycle.phase == SPEAK_IDLE && cycle.outcome == SPEAK_OUTCOME_DONE);
    return 0;
}
