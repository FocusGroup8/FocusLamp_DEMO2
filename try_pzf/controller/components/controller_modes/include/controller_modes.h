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
    uint32_t revision; /* Changes on mode/session transitions, not voice pauses. */
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
const char *controller_modes_name(app_state_t mode);

/* Lock order inside the component is modes -> device_state. A consumer must
 * not call this component while holding the device_state lock. */

#ifdef __cplusplus
}
#endif
