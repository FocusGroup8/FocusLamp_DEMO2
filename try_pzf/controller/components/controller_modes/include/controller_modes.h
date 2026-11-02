#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "device_state.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* State/countdown only: no lighting, LCD, audio, servo or head-side effects. */
typedef struct {
    app_state_t mode;
    uint32_t duration_sec;
    uint32_t remaining_sec;
    bool voice_active;
    bool paused;
    bool completed;
    uint32_t uptime_ms;
    uint32_t duration_ms, remaining_ms, controller_boot_id, session_id, seq, engagement_token;
    int rate_quarters; /* Exact active speed: 0/4, 3/4 or 4/4. */
    double rate_half; /* Legacy /2 field, now allows 1.5; prefer rate_quarters. */
    unsigned pause_reason;
    uint32_t revision; /* Changes on mode/session transitions, not voice pauses. */
    uint32_t full_ms, normal_ms, held_ms;
    uint32_t half_ms; /* Legacy 0.5x residence: always zero under this policy. */
    uint32_t held_waiting_ms, held_distracted_ms, held_invalid_ms, held_stale_ms, held_voice_ms;
} controller_modes_status_t;

esp_err_t controller_modes_init(void);
/* Minutes must be 1..180. Repeated focus start does not reset a session. */
esp_err_t controller_modes_focus_start(uint32_t minutes);
/* Typed stops are idempotent and never stop another active mode. */
esp_err_t controller_modes_focus_stop(void);
esp_err_t controller_modes_companion_start(void);
esp_err_t controller_modes_companion_stop(void);
/* Explicit idle: clears any session and its completion marker. */
esp_err_t controller_modes_stop(void);

/* Available before init; initialization preserves this latest chat state. */
void controller_modes_set_voice_active(bool active);
bool controller_modes_voice_active(void);
esp_err_t controller_modes_get_status(controller_modes_status_t *out);
const char *controller_modes_pause_name(unsigned reason);
esp_err_t controller_modes_engagement(uint32_t boot, uint32_t session, uint32_t token,
    bool bind, uint32_t sample, uint32_t inference, uint32_t age_ms, uint32_t sample_age_ms,
    bool valid, const char *level);
const char *controller_modes_name(app_state_t mode);

/* Lock order inside the component is modes -> device_state. A consumer must
 * not call this component while holding the device_state lock. */

#ifdef __cplusplus
}
#endif
