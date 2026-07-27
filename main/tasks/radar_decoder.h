/*
 * radar_decoder.h - 雷达帧数据解码接口
 *
 * 轻量封装，底层委托给 radar_driver_decode_* 函数。
 */

#pragma once
#ifndef __RADAR_DECODER_H__
#define __RADAR_DECODER_H__

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 从帧中解码布尔值（取 data[0]）
 */
bool radar_decoder_decode_bool(const void *frame);

/**
 * @brief 从帧数据偏移处解码小端 float
 */
float radar_decoder_decode_float_at(const void *frame, size_t offset);

/**
 * @brief 从帧数据偏移处解码小端 uint32
 */
uint32_t radar_decoder_decode_uint32_at(const void *frame, size_t offset);

/**
 * @brief 从帧数据偏移处解码小端 int32
 */
int32_t radar_decoder_decode_int32_at(const void *frame, size_t offset);

#ifdef __cplusplus
}
#endif

#endif /* __RADAR_DECODER_H__ */
