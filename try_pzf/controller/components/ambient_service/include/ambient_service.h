#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AMBIENT_SAMPLE_INTERVAL_MS 1000
#define AMBIENT_BLOCK_INTERVAL_MS 2000
#define AMBIENT_STALE_TIMEOUT_MS 10000

/* All times use the controller's monotonic uptime, not wall-clock/Unix time.
 * valid means a sample has existed; only fresh samples may drive adaptation.
 * last_ok_ms is the last successful ADC batch incorporated in lux, never the
 * time a reader republishes the cached value. age_ms=-1 before first sample. */
typedef struct {
    bool valid;
    bool fresh;
    float lux;                 /* Estimated, uncalibrated; EMA filtered. */
    uint8_t level;             /* 0..4; zero is a legitimate dark reading. */
    int64_t last_ok_ms;
    int64_t age_ms;
} ambient_snapshot_t;

/* Call from the startup task. Starts one low-priority, 1 Hz sampling task;
 * repeated calls after success are idempotent. Failure does not start a task. */
esp_err_t ambient_service_start(void);

/* Short snapshot copy only: never reads ADC, takes the LED mutex, or does I/O.
 * ESP_OK can carry valid=false/fresh=false. Callers must check fresh. */
esp_err_t ambient_service_get_snapshot(ambient_snapshot_t *out);

#ifdef __cplusplus
}
#endif
