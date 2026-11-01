#ifndef WS_CAMERA_LEASE_H
#define WS_CAMERA_LEASE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* TCP keepalive only reaches a proxy, which may outlive the actual viewer.
 * One outstanding RFC6455 Ping tests the camera consumer end to end. */
#define WS_CAMERA_PING_INTERVAL_US INT64_C(5000000)
#define WS_CAMERA_PONG_TIMEOUT_US INT64_C(15000000)
#define WS_CAMERA_PING_BYTES 8

typedef struct {
    uint32_t session_id;
    uint32_t challenge;
    int64_t next_ping_us;
    int64_t deadline_us;
    bool awaiting_pong;
} ws_camera_lease_t;

static inline void ws_camera_lease_start(ws_camera_lease_t *lease, uint32_t session_id, int64_t now_us)
{
    *lease = (ws_camera_lease_t){.session_id = session_id,
        .next_ping_us = now_us + WS_CAMERA_PING_INTERVAL_US};
}

static inline bool ws_camera_lease_expired(const ws_camera_lease_t *lease, int64_t now_us)
{
    return lease->awaiting_pong && now_us >= lease->deadline_us;
}

static inline bool ws_camera_lease_ping_due(const ws_camera_lease_t *lease, int64_t now_us)
{
    return !lease->awaiting_pong && now_us >= lease->next_ping_us;
}

static inline void ws_camera_lease_payload(const ws_camera_lease_t *lease,
                                           uint8_t payload[WS_CAMERA_PING_BYTES])
{
    for (unsigned i = 0; i < 4; ++i) {
        payload[i] = (uint8_t)(lease->session_id >> (24 - 8 * i));
        payload[i + 4] = (uint8_t)(lease->challenge >> (24 - 8 * i));
    }
}

/* Called once, under the same send mutex that protects the entire Ping frame.
 * Do not replace the challenge until its matching Pong or its timeout. */
static inline void ws_camera_lease_ping_started(ws_camera_lease_t *lease, int64_t now_us,
                                                uint8_t payload[WS_CAMERA_PING_BYTES])
{
    ++lease->challenge;
    lease->awaiting_pong = true;
    lease->deadline_us = now_us + WS_CAMERA_PONG_TIMEOUT_US;
    lease->next_ping_us = now_us + WS_CAMERA_PING_INTERVAL_US;
    ws_camera_lease_payload(lease, payload);
}

static inline bool ws_camera_lease_pong(ws_camera_lease_t *lease, const uint8_t *payload,
                                       size_t len, int64_t now_us)
{
    uint8_t expected[WS_CAMERA_PING_BYTES];
    if (!lease->awaiting_pong || ws_camera_lease_expired(lease, now_us) ||
        len != WS_CAMERA_PING_BYTES || payload == NULL) return false;
    ws_camera_lease_payload(lease, expected);
    if (memcmp(payload, expected, sizeof(expected)) != 0) return false;
    lease->awaiting_pong = false;
    return true;
}

#endif
