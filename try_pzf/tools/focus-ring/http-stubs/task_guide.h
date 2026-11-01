#pragma once
#include "cJSON.h"
#include "esp_err.h"
/* Route-contract fixture only. Gate behavior has a separate production-C test. */
cJSON *task_guide_snapshot(void);
esp_err_t task_guide_report(const cJSON *report);
