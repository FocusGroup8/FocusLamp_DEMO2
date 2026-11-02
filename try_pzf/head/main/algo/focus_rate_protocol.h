#pragma once
#include <math.h>
#include <stdbool.h>
#include "cJSON.h"

/* New authority uses exact quarters. Accept old /2 replies during an upgrade,
 * but never fall back from an explicitly malformed new field to old data. */
static inline bool focus_rate_quarters(const cJSON *reply, int *out)
{
    const cJSON *q = cJSON_GetObjectItemCaseSensitive(reply, "rate_quarters");
    if (q) {
        if (!cJSON_IsNumber(q) || !isfinite(q->valuedouble) ||
            (q->valuedouble != 0 && q->valuedouble != 3 && q->valuedouble != 4)) return false;
        *out = (int)q->valuedouble;
        return true;
    }
    const cJSON *half = cJSON_GetObjectItemCaseSensitive(reply, "rate_half");
    if (!cJSON_IsNumber(half) || !isfinite(half->valuedouble) ||
        (half->valuedouble != 0 && half->valuedouble != 1 &&
         half->valuedouble != 1.5 && half->valuedouble != 2)) return false;
    *out = (int)(half->valuedouble * 2);
    return true;
}
