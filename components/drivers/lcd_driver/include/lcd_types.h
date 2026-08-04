/*
 * lcd_types.h - LCD type definitions for FocusLamp
 */

#pragma once
#ifndef __LCD_TYPES_H__
#define __LCD_TYPES_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LCD_COLOR_WHITE   0xFFFF
#define LCD_COLOR_BLACK   0x0000
#define LCD_COLOR_RED     0xF800
#define LCD_COLOR_GREEN   0x07E0
#define LCD_COLOR_BLUE    0x001F
#define LCD_COLOR_YELLOW  0xFFE0
#define LCD_COLOR_CYAN    0x7FFF
#define LCD_COLOR_MAGENTA 0xF81F

typedef uint16_t lcd_color_t;

/* ===================== Page Enumeration ===================== */
typedef enum {
    LCD_PAGE_EXPRESSION = 0,
    LCD_PAGE_INFO       = 1,
    LCD_PAGE_COUNT      = 2,
} lcd_page_t;

/* ===================== Expression Enumeration ===================== */
typedef enum {
    LCD_EXPRESSION_NORMAL = 0,
    LCD_EXPRESSION_HAPPY,
    LCD_EXPRESSION_SAD,
    LCD_EXPRESSION_ANGRY,
    LCD_EXPRESSION_SURPRISED,
    LCD_EXPRESSION_SLEEPY,
    LCD_EXPRESSION_COUNT,
} lcd_expression_t;

/* ===================== Eye Configuration ===================== */
typedef struct {
    int16_t offset_x;
    int16_t offset_y;
    int16_t height;
    int16_t width;
    float   slope_top;
    float   slope_bottom;
    int16_t radius_top;
    int16_t radius_bottom;
    int16_t inverse_radius_top;
    int16_t inverse_radius_bottom;
    int16_t inverse_offset_top;
    int16_t inverse_offset_bottom;
} lcd_eye_config_t;

#ifdef __cplusplus
}
#endif

#endif /* __LCD_TYPES_H__ */
