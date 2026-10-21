/* Host fixture: tests the same private C header included by the manager. */
#include "../components/xiaozhi_manager/src/uplink_buffer.h"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

/* The Windows host test links without a CRT or Windows SDK. */
#ifdef UPLINK_TEST_FREESTANDING
void *memcpy(void *destination, const void *source, size_t len)
{
    unsigned char *out = destination;
    const unsigned char *in = source;
    for (size_t i = 0; i < len; ++i) {
        out[i] = in[i];
    }
    return destination;
}

void *memset(void *destination, int value, size_t len)
{
    unsigned char *out = destination;
    for (size_t i = 0; i < len; ++i) {
        out[i] = (unsigned char)value;
    }
    return destination;
}
#endif

static void fill(uint8_t *data, size_t len, unsigned seed)
{
    for (size_t i = 0; i < len; ++i) {
        data[i] = (uint8_t)(seed + i);
    }
}

static bool matches(const uplink_frame_t *frame, size_t len,
                    uint32_t generation, int64_t enqueued_us, unsigned seed)
{
    if (frame->len != len || frame->generation != generation ||
        frame->enqueued_us != enqueued_us) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        if (frame->data[i] != (uint8_t)(seed + i)) {
            return false;
        }
    }
    return true;
}

int uplink_buffer_test(void)
{
    uplink_buffer_t queue = {0};
    uplink_frame_t out = {0};
    uplink_frame_t held = {0};
    uplink_frame_t time_frame = {0};
    uint8_t source[UPLINK_FRAME_BYTES];
    bool dropped = true;

    CHECK(UPLINK_FRAME_BYTES == 1500 && UPLINK_QUEUE_DEPTH == 8);
    CHECK(!uplink_buffer_pop(&queue, &out));
    CHECK(uplink_buffer_clear(&queue) == 0);
    CHECK(!uplink_buffer_push(&queue, source, 0, 1, 0, &dropped));
    CHECK(!dropped && queue.count == 0);
    CHECK(!uplink_buffer_push(&queue, source, UPLINK_FRAME_BYTES + 1, 1, 0, &dropped));
    CHECK(!uplink_buffer_push(&queue, source, (size_t)-1, 1, 0, &dropped));
    CHECK(!uplink_buffer_push(&queue, NULL, 1, 1, 0, &dropped));
    CHECK(!uplink_buffer_push(NULL, source, 1, 1, 0, &dropped));
    CHECK(!uplink_buffer_pop(NULL, &out));
    CHECK(!uplink_buffer_pop(&queue, NULL));
    CHECK(uplink_buffer_clear(NULL) == 0);
    CHECK(queue.count == 0 && queue.head == 0);

    /* Source-buffer reuse cannot mutate an accepted frame; lengths stay exact. */
    fill(source, sizeof(source), 17);
    CHECK(uplink_buffer_push(&queue, source, sizeof(source), 4, 0, &dropped));
    CHECK(!dropped);
    fill(source, sizeof(source), 99);
    CHECK(uplink_buffer_push(&queue, source, 1, 5, 1000, NULL));
    CHECK(!uplink_buffer_pop(&queue, NULL) && queue.count == 2);
    CHECK(uplink_buffer_pop(&queue, &held));
    CHECK(matches(&held, sizeof(source), 4, 0, 17));
    CHECK(uplink_frame_dwell_ms(&held, 0) == 0);
    CHECK(uplink_frame_dwell_ms(&held, 999) == 0);
    CHECK(uplink_frame_dwell_ms(&held, 1000) == 1);
    CHECK(uplink_frame_dwell_ms(&held, 481000) == 481);
    CHECK(uplink_buffer_pop(&queue, &out));
    CHECK(matches(&out, 1, 5, 1000, 99));
    CHECK(!uplink_buffer_pop(&queue, &out));

    /* Overwrite drops exactly the oldest frame and preserves FIFO ordering. */
    for (unsigned i = 0; i < UPLINK_QUEUE_DEPTH + 3; ++i) {
        fill(source, 3, i);
        CHECK(uplink_buffer_push(&queue, source, 3, 8,
                                 INT64_C(5000000000) + i * 60000, &dropped));
        CHECK(dropped == (i >= UPLINK_QUEUE_DEPTH));
        CHECK(queue.count == (i < UPLINK_QUEUE_DEPTH ? i + 1 : UPLINK_QUEUE_DEPTH));
    }
    /* Rejecting invalid input on a full queue must not discard a frame. */
    CHECK(!uplink_buffer_push(&queue, NULL, 3, 9, 0, &dropped));
    CHECK(!dropped && queue.count == UPLINK_QUEUE_DEPTH);
    for (unsigned i = 3; i < UPLINK_QUEUE_DEPTH + 3; ++i) {
        CHECK(uplink_buffer_pop(&queue, &out));
        CHECK(matches(&out, 3, 8, INT64_C(5000000000) + i * 60000, i));
    }
    CHECK(!uplink_buffer_pop(&queue, &out));
    CHECK(matches(&held, sizeof(source), 4, 0, 17));

    /* Clearing removes all pending generations and resets both indices. */
    fill(source, 2, 40);
    CHECK(uplink_buffer_push(&queue, source, 2, 10, 300, NULL));
    CHECK(uplink_buffer_push(&queue, source, 2, 10, 400, NULL));
    CHECK(uplink_buffer_clear(&queue) == 2);
    CHECK(queue.count == 0 && queue.head == 0);
    CHECK(!uplink_buffer_pop(&queue, &out));
    fill(source, 2, 41);
    CHECK(uplink_buffer_push(&queue, source, 2, 11, 500, NULL));
    CHECK(uplink_buffer_pop(&queue, &out));
    CHECK(matches(&out, 2, 11, 500, 41));

    /* Dwell is a pure measurement; negative/future stamps are unknown. */
    CHECK(uplink_frame_dwell_ms(NULL, 0) == -1);
    time_frame.enqueued_us = -1;
    CHECK(uplink_frame_dwell_ms(&time_frame, 0) == -1);
    time_frame.enqueued_us = INT64_C(5000000000);
    CHECK(uplink_frame_dwell_ms(&time_frame, INT64_C(4999999999)) == -1);
    CHECK(uplink_frame_dwell_ms(&time_frame, INT64_C(5000000000)) == 0);
    CHECK(uplink_frame_dwell_ms(&time_frame, INT64_C(5000600999)) == 600);
    time_frame.enqueued_us = 0;
    CHECK(uplink_frame_dwell_ms(&time_frame, -1) == -1);
    CHECK(uplink_frame_dwell_ms(&time_frame, INT64_MAX) == INT64_MAX / 1000);

    /* Exercise many wraparounds with interleaved writes and reads. */
    for (unsigned i = 0; i < 10000; ++i) {
        fill(source, sizeof(source), i);
        CHECK(uplink_buffer_push(&queue, source, sizeof(source), 20 + i,
                                 INT64_C(6000000000) + i * 60000, &dropped));
        CHECK(queue.count <= UPLINK_QUEUE_DEPTH && queue.head < UPLINK_QUEUE_DEPTH);
        if (i % 3 == 2) {
            unsigned expected = i < 9 ? i / 3 : i - (UPLINK_QUEUE_DEPTH - 1);
            CHECK(uplink_buffer_pop(&queue, &out));
            CHECK(matches(&out, sizeof(source), 20 + expected,
                          INT64_C(6000000000) + expected * 60000, expected));
        }
        CHECK(matches(&held, sizeof(source), 4, 0, 17));
    }
    CHECK(queue.count == UPLINK_QUEUE_DEPTH);
    for (unsigned i = 10000 - UPLINK_QUEUE_DEPTH; i < 10000; ++i) {
        CHECK(uplink_buffer_pop(&queue, &out));
        CHECK(matches(&out, sizeof(source), 20 + i,
                      INT64_C(6000000000) + i * 60000, i));
    }
    CHECK(!uplink_buffer_pop(&queue, &out));
    CHECK(uplink_buffer_clear(&queue) == 0);
    return 0;
}
