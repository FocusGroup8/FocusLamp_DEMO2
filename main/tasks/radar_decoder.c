/*
 * radar_decoder.c - 雷达帧数据解码实现
 *
 * 轻量封装，底层委托给 radar_driver 的解码函数。
 */

#include "radar_decoder.h"
#include "radar_driver.h"

bool radar_decoder_decode_bool(const void *frame)
{
    return radar_driver_decode_bool(frame);
}

float radar_decoder_decode_float_at(const void *frame, size_t offset)
{
    return radar_driver_decode_float_at(frame, offset);
}

uint32_t radar_decoder_decode_uint32_at(const void *frame, size_t offset)
{
    return radar_driver_decode_uint32_at(frame, offset);
}

int32_t radar_decoder_decode_int32_at(const void *frame, size_t offset)
{
    return radar_driver_decode_int32_at(frame, offset);
}
