#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    bool focus, paused, completed, fresh;
    uint32_t boot, session, seq, duration_ms, remaining_ms;
    int rate_quarters;
    char reason[24];
    int64_t received_us;
} focus_view_t;
esp_err_t focus_sync_start(void);
void focus_sync_source(int client_fd, const char *epoch);
void focus_sync_sample(int client_fd, const cJSON *focus);
void focus_sync_get_view(focus_view_t *out);
cJSON *focus_sync_diagnostics(void);
#ifdef __cplusplus
}
#endif
