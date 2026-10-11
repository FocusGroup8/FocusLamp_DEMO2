/*
 * lcd_font.h - ASCII font data for LCD display
 */

#pragma once
#ifndef __LCD_FONT_H__
#define __LCD_FONT_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const uint8_t ascii_1206[][12];
extern const uint8_t ascii_1608[][16];

#ifdef __cplusplus
}
#endif

#endif /* __LCD_FONT_H__ */
