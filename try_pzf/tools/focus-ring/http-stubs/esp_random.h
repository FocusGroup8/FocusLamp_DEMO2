#pragma once
#include <stdint.h>
/* Longest uint32 identities exercise JSON formatting near its upper bounds. */
static inline uint32_t esp_random(void) { static uint32_t next=UINT32_MAX; return next--; }
