#ifndef XIAOZHI_UPLINK_BUFFER_H
#define XIAOZHI_UPLINK_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UPLINK_FRAME_BYTES 1500
#define UPLINK_QUEUE_DEPTH 8

typedef struct {
    uint32_t generation;
    int64_t enqueued_us;
    uint16_t len;
    uint8_t data[UPLINK_FRAME_BYTES];
} uplink_frame_t;

typedef struct {
    uplink_frame_t frames[UPLINK_QUEUE_DEPTH];
    unsigned head;
    unsigned count;
} uplink_buffer_t;

/* Initialize to zero. The caller must synchronize all access to the buffer. */
static inline unsigned uplink_buffer_clear(uplink_buffer_t *buffer)
{
    if (buffer == NULL) {
        return 0;
    }
    unsigned removed = buffer->count;
    buffer->head = 0;
    buffer->count = 0;
    return removed;
}

static inline bool uplink_buffer_push(uplink_buffer_t *buffer,
                                      const uint8_t *data, size_t len,
                                      uint32_t generation, int64_t enqueued_us,
                                      bool *dropped_oldest)
{
    if (dropped_oldest != NULL) {
        *dropped_oldest = false;
    }
    if (buffer == NULL || data == NULL || len == 0 || len > UPLINK_FRAME_BYTES) {
        return false;
    }

    if (buffer->count == UPLINK_QUEUE_DEPTH) {
        buffer->head = (buffer->head + 1) % UPLINK_QUEUE_DEPTH;
        --buffer->count;
        if (dropped_oldest != NULL) {
            *dropped_oldest = true;
        }
    }

    unsigned tail = (buffer->head + buffer->count) % UPLINK_QUEUE_DEPTH;
    uplink_frame_t *frame = &buffer->frames[tail];
    frame->generation = generation;
    frame->enqueued_us = enqueued_us;
    frame->len = (uint16_t)len;
    for (size_t i = 0; i < len; ++i) {
        frame->data[i] = data[i];
    }
    ++buffer->count;
    return true;
}

/* The returned frame owns its bytes and is independent of future queue writes. */
static inline bool uplink_buffer_pop(uplink_buffer_t *buffer, uplink_frame_t *out)
{
    if (buffer == NULL || out == NULL || buffer->count == 0) {
        return false;
    }
    *out = buffer->frames[buffer->head];
    buffer->head = (buffer->head + 1) % UPLINK_QUEUE_DEPTH;
    --buffer->count;
    return true;
}

/* Diagnostic only: invalid or future timestamps have no measurable dwell. */
static inline int64_t uplink_frame_dwell_ms(const uplink_frame_t *frame,
                                           int64_t now_us)
{
    if (frame == NULL || frame->enqueued_us < 0 || now_us < frame->enqueued_us) {
        return -1;
    }
    return (now_us - frame->enqueued_us) / 1000;
}

#endif
