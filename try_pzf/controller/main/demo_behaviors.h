#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool started;
    uint32_t revision;
    bool companion_local_ready;
    bool head_sync_ready; /* HTTP 200 + code=0, not physical confirmation. */
    bool radar_fresh;
    bool radar_present;
    uint32_t sedentary_sec;
    bool sedentary_pending;
    bool sedentary_armed;
    bool sedentary_submitted; /* Queued locally, not confirmed playback. */
    bool sedentary_text_observed; /* Legacy cloud diagnostic; false for local clips. */
    bool sedentary_local_output_done; /* PCM + silence tail written, not audibility. */
} demo_behaviors_status_t;

/* App-lifetime worker, priority 3; call after voice and controller_modes init. */
esp_err_t demo_behaviors_start(void);
void demo_behaviors_get_status(demo_behaviors_status_t *out);
/* Consumes callback-lifetime text synchronously; never retains its pointer. */
void demo_behaviors_note_tts_text(const char *text);
