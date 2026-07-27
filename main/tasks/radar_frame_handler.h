/*
 * radar_frame_handler.h - 雷达帧处理接口
 *
 * 解析 TF 帧并通过 event_bus 发布雷达事件。
 */

#pragma once
#ifndef __RADAR_FRAME_HANDLER_H__
#define __RADAR_FRAME_HANDLER_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct tf_frame;

void radar_frame_handler_init(void);
void radar_frame_handler_handle(const struct tf_frame *frame, uint32_t elapsed_ms);

#ifdef __cplusplus
}
#endif

#endif /* __RADAR_FRAME_HANDLER_H__ */
