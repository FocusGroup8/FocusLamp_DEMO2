/*
 * lcd_font_cn.h - 16x16 Chinese font for LCD display
 *
 * Auto-generated from SimHei font. Supports a predefined set of characters.
 * Unknown characters display as a placeholder box.
 */
#pragma once
#ifndef __LCD_FONT_CN_H__
#define __LCD_FONT_CN_H__

#include <stdint.h>

/* One Chinese character: 16 rows of 16 bits */
typedef struct {
    uint32_t utf8_key;              /* UTF-8 3-byte packed key */
    const uint16_t *bitmap;         /* 16 x uint16_t row data */
} lcd_cn_char_t;

/* Font table (sorted for binary search) */
extern const lcd_cn_char_t lcd_cn_font[];
extern const int lcd_cn_font_count;

/**
 * @brief Look up a Chinese character bitmap by UTF-8 3-byte key.
 * @param key  UTF-8 3 bytes packed as (b0 << 16) | (b1 << 8) | b2
 * @return Pointer to 16x uint16_t bitmap, or NULL if not found.
 */
const uint16_t *lcd_cn_lookup(uint32_t key);

#endif /* __LCD_FONT_CN_H__ */
