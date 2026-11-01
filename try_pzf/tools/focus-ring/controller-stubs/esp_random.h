#pragma once
#include <stdint.h>
static inline uint32_t esp_random(void){static uint32_t x=100;return ++x;}
