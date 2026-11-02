#pragma once

#include <math.h>
#include <stdint.h>

/* Values are linear ISP samples before RGB565 quantization and JPEG encoding.
 * Black and white endpoints stay fixed. Level 1 is the original mild curve. */
static inline uint32_t camera_tone_value(int level, uint32_t x)
{
    if (x == 0) return 0;
    if (x >= 255) return 255;
    if (level == 1) return x + (3U * x * (255U - x) + 1020U) / 2040U;
    if (level == 2) return (uint32_t)lroundf(255.0f * powf(x / 255.0f, 0.70f));
    if (level == 3) return (uint32_t)lroundf(255.0f * powf(x / 255.0f, 0.55f));
    return x;
}
