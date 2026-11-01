#pragma once

#include "cJSON.h"
#include "esp_err.h"
#include "esp_mcp_engine.h"
#include <stddef.h>

#define TASK_GUIDE_REQUEST_PROMPT_BYTES 1024

/* Call for USER STT only, never for assistant/TTS text. Does not open a mode. */
void task_guide_note_user_text(const char *text);
/* Explicit HTTP text requests use this immediately before cloud submission.
 * NOT_FOUND: ordinary text; INVALID_STATE: unsupported mode / active guide;
 * INVALID_SIZE: prompt does not fit. It never changes the application mode. */
esp_err_t task_guide_prepare_request(const char *text, char *prompt, size_t capacity);
/* Cancel only the matching still-waiting request after failed submission. */
void task_guide_cancel_prepared_request(const char *prepared_prompt);
/* Allocated JSON snapshot owned by caller. Also expires a pending request. */
cJSON *task_guide_snapshot(void);
/* Head is the sole clock. Reports only record state, never advance a timer. */
esp_err_t task_guide_report(const cJSON *report);
/* Called for both initial and recreated MCP engines. */
esp_err_t task_guide_register_mcp_tools(esp_mcp_t *engine);
