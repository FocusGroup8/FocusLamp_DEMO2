#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef bool (*local_reminder_allowed_t)(uint32_t cookie);
typedef void (*local_reminder_result_t)(uint32_t cookie, esp_err_t result);
esp_err_t local_reminder_start(void);
/* Queue receipt only. allowed is checked again by the playback task. */
esp_err_t local_reminder_submit(uint32_t cookie, local_reminder_allowed_t allowed,
                                local_reminder_result_t result);

/* Only the head's three legacy welcome commands use this local route. */
bool local_welcome_matches(const char *text);
/* Accepted/coalesced, not proof of audible playback. At most one pending. */
esp_err_t local_welcome_submit(void);
/* Nonblocking: drops pending and cooperatively cancels the active welcome.
 * Does not cancel an unrelated radar reminder. */
void local_welcome_cancel(void);
