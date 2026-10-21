/* Actual-header assertions. A cross-compiler -c check validates syntax/API only;
 * assertions execute only when a native runner calls the exported test or
 * builds this file with -DLOCAL_WELCOME_POLICY_TEST_MAIN and runs its main. */
#include "../main/local_welcome_policy.h"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

int local_welcome_policy_test(void)
{
    {
        local_welcome_policy_t s = {0};
        uint32_t generation = 0;
        uint8_t index = 0;
        CHECK(!local_welcome_policy_take(&s, &generation, &index));
        CHECK(local_welcome_policy_cancelled(&s, 0));
        CHECK(!local_welcome_policy_start(&s, 0));
        for (unsigned item = 0; item < LOCAL_WELCOME_COUNT * 2; ++item) {
            local_welcome_policy_request(&s);
            CHECK(local_welcome_policy_take(&s, &generation, &index));
            CHECK(index == item % LOCAL_WELCOME_COUNT);
            CHECK(s.next_index == index);
            CHECK(local_welcome_policy_start(&s, generation));
            CHECK(!local_welcome_policy_start(&s, generation));
            CHECK(s.next_index == (item + 1) % LOCAL_WELCOME_COUNT);
            local_welcome_policy_finish(&s, generation);
            CHECK(!s.active && !s.started);
        }
        CHECK(s.next_index == 0);
    }
    {
        local_welcome_policy_t s = {0};
        uint32_t first = 0;
        uint32_t second = 0;
        uint8_t index = 0;
        for (unsigned burst = 0; burst < 20; ++burst) {
            local_welcome_policy_request(&s);
        }
        CHECK(local_welcome_policy_take(&s, &first, &index));
        CHECK(index == 0 && !s.pending);
        for (unsigned burst = 0; burst < 20; ++burst) {
            local_welcome_policy_request(&s);
        }
        CHECK(!local_welcome_policy_take(&s, &second, &index));
        CHECK(local_welcome_policy_output_started(&s, first));
        local_welcome_policy_finish(&s, first);
        CHECK(local_welcome_policy_take(&s, &second, &index));
        CHECK(index == 1 && !s.pending);
        CHECK(local_welcome_policy_output_started(&s, second));
        local_welcome_policy_finish(&s, first); /* Late previous cleanup. */
        CHECK(s.active && s.started);
        local_welcome_policy_finish(&s, second);
        CHECK(!local_welcome_policy_take(&s, &first, &index));
    }
    {
        local_welcome_policy_t s = {0};
        uint32_t cancelled = 0;
        uint32_t replacement = 0;
        uint8_t index = 0;
        local_welcome_policy_request(&s);
        local_welcome_policy_cancel(&s); /* Pending request never skipped item 1. */
        CHECK(!s.pending && s.next_index == 0);
        CHECK(!local_welcome_policy_take(&s, &cancelled, &index));
        local_welcome_policy_request(&s);
        CHECK(local_welcome_policy_take(&s, &cancelled, &index));
        local_welcome_policy_cancel(&s); /* Cancel before first PCM write. */
        CHECK(s.active && s.next_index == 0);
        CHECK(local_welcome_policy_cancelled(&s, cancelled));
        CHECK(!local_welcome_policy_start(&s, cancelled));
        local_welcome_policy_request(&s);
        CHECK(!local_welcome_policy_take(&s, &replacement, &index));
        /* No output report: bridge did not write PCM. */
        local_welcome_policy_finish(&s, cancelled);
        CHECK(s.pending && !s.active && s.next_index == 0);
        CHECK(local_welcome_policy_take(&s, &replacement, &index));
        CHECK(replacement != cancelled && index == 0);
        CHECK(!local_welcome_policy_output_started(&s, cancelled));
        CHECK(!s.started && s.next_index == 0);
        CHECK(local_welcome_policy_output_started(&s, replacement));
        local_welcome_policy_finish(&s, replacement);
    }
    {
        local_welcome_policy_t s = {0};
        uint32_t generation = 0;
        uint8_t index = 0;
        local_welcome_policy_request(&s);
        CHECK(local_welcome_policy_take(&s, &generation, &index));
        /* PCM was written, then cancellation arrived before bridge return. */
        local_welcome_policy_cancel(&s);
        local_welcome_policy_request(&s);
        CHECK(!local_welcome_policy_start(&s, generation));
        CHECK(local_welcome_policy_output_started(&s, generation));
        CHECK(s.started && s.next_index == 1);
        CHECK(!local_welcome_policy_output_started(&s, generation));
        CHECK(s.next_index == 1);
        local_welcome_policy_finish(&s, generation);
        CHECK(s.pending && !s.active && !s.started);
        CHECK(local_welcome_policy_take(&s, &generation, &index));
        CHECK(index == 1);
    }
    {
        local_welcome_policy_t s = {0};
        uint32_t generation = 99;
        s.generation = UINT32_MAX;
        local_welcome_policy_request(&s);
        CHECK(local_welcome_policy_take(&s, &generation, 0));
        CHECK(generation == 0);
        CHECK(!local_welcome_policy_cancelled(&s, generation));
        local_welcome_policy_cancel(&s);
        CHECK(local_welcome_policy_cancelled(&s, generation));
        local_welcome_policy_finish(&s, generation);
        CHECK(!s.active);
        CHECK(local_welcome_policy_cancelled(0, 0));
        CHECK(!local_welcome_policy_take(0, 0, 0));
        CHECK(!local_welcome_policy_start(0, 0));
        CHECK(!local_welcome_policy_output_started(0, 0));
        local_welcome_policy_request(0);
        local_welcome_policy_cancel(0);
        local_welcome_policy_finish(0, 0);
    }
    return 0;
}

#ifdef LOCAL_WELCOME_POLICY_TEST_MAIN
int main(void)
{
    return local_welcome_policy_test();
}
#endif
