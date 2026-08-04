/*
 * radar_breath_handler.h - 呼吸相位处理接口
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void radar_breath_handler_init(void);

void radar_breath_handler_update(float phase, uint32_t elapsed_ms);

bool radar_breath_handler_filter_phase(float phase, uint32_t elapsed_ms);

float radar_breath_handler_get_filtered_phase(void);

bool radar_breath_handler_has_phase(void);

#ifdef __cplusplus
}
#endif