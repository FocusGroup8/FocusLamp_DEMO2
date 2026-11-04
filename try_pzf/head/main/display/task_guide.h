#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Called only for complete successful controller snapshots. No network owns the clock. */
void task_guide_observe_mode(const cJSON *snapshot);
void task_guide_set_touch_available(bool available);
/* Existing LVGL GT911 input is the only screen reader. No second I2C poller. */
void task_guide_touch_pressed(void);
void task_guide_touch_released(void);
void task_guide_touch_cancelled(void);
bool task_guide_report(char *json, size_t capacity, uint32_t *sequence);
void task_guide_report_ack(uint32_t sequence);
#ifdef __cplusplus
}
/* Returns true when the task overlay was drawn onto the existing canvas.
 * Consecutive true frames repaint only what changed, so nothing else may
 * draw on the canvas between them (a false frame resets this). */
bool task_guide_draw(uint8_t *bytes, int width, int height, int stride, int64_t now_ms);
/* Optional: builds eye sprites and the track table (~100 ms) ahead of the
 * first guide, e.g. at init while the backlight is still off. */
void task_guide_prepare(uint8_t *bytes, int width, int height, int stride);
#endif
