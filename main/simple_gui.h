/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_lcd_panel_ops.h"
#include <stdint.h>

/**
 * @brief Simple GUI drawing library for LCD panels
 * 
 * This library provides basic drawing functions without LVGL dependency.
 * Supports RGB888 color format.
 */

typedef struct {
    esp_lcd_panel_handle_t panel;
    uint16_t width;
    uint16_t height;
} simple_gui_t;

// Color definitions (RGB888)
#define COLOR_RED       0xFF0000
#define COLOR_GREEN     0x00FF00
#define COLOR_BLUE      0x0000FF
#define COLOR_WHITE     0xFFFFFF
#define COLOR_BLACK     0x000000
#define COLOR_YELLOW    0xFFFF00
#define COLOR_CYAN      0x00FFFF
#define COLOR_MAGENTA   0xFF00FF
#define COLOR_ORANGE    0xFF8000
#define COLOR_GRAY      0x808080

/**
 * @brief Initialize GUI context
 */
void simple_gui_init(simple_gui_t *gui, esp_lcd_panel_handle_t panel, uint16_t width, uint16_t height);

/**
 * @brief Clear screen to a specific color
 */
void gui_clear_screen(simple_gui_t *gui, uint32_t color);

/**
 * @brief Draw a pixel at specified position
 */
void gui_draw_pixel(simple_gui_t *gui, int x, int y, uint32_t color);

/**
 * @brief Draw a line between two points
 */
void gui_draw_line(simple_gui_t *gui, int x1, int y1, int x2, int y2, uint32_t color);

/**
 * @brief Draw a horizontal line
 */
void gui_draw_hline(simple_gui_t *gui, int x, int y, int length, uint32_t color);

/**
 * @brief Draw a vertical line
 */
void gui_draw_vline(simple_gui_t *gui, int x, int y, int length, uint32_t color);

/**
 * @brief Draw a filled rectangle
 */
void gui_draw_filled_rect(simple_gui_t *gui, int x1, int y1, int x2, int y2, uint32_t color);

/**
 * @brief Draw a rectangle outline
 */
void gui_draw_rect_outline(simple_gui_t *gui, int x1, int y1, int x2, int y2, uint32_t color, uint8_t thickness);

/**
 * @brief Draw a filled circle
 */
void gui_draw_filled_circle(simple_gui_t *gui, int cx, int cy, int radius, uint32_t color);

/**
 * @brief Draw a circle outline
 */
void gui_draw_circle_outline(simple_gui_t *gui, int cx, int cy, int radius, uint32_t color, uint8_t thickness);

/**
 * @brief Draw a simple 8x8 bitmap character (ASCII)
 */
void gui_draw_char(simple_gui_t *gui, int x, int y, char ch, uint32_t color, uint32_t bg_color, uint8_t size);

/**
 * @brief Draw a text string
 */
void gui_draw_string(simple_gui_t *gui, int x, int y, const char *str, uint32_t color, uint32_t bg_color, uint8_t size);

/**
 * @brief Draw a number
 */
void gui_draw_number(simple_gui_t *gui, int x, int y, int num, uint32_t color, uint32_t bg_color, uint8_t size);

/**
 * @brief Fill screen with gradient pattern
 */
void gui_draw_gradient(simple_gui_t *gui, uint32_t start_color, uint32_t end_color, bool horizontal);

/**
 * @brief Draw a test pattern grid
 */
void gui_draw_grid_test(simple_gui_t *gui, int grid_size);

/**
 * @brief Invert display colors
 */
void gui_invert_display(simple_gui_t *gui, bool invert);

/**
 * @brief Mirror display (X or Y axis)
 */
void gui_mirror_display(simple_gui_t *gui, bool mirror_x, bool mirror_y);

/**
 * @brief Swap display X and Y axes (rotation)
 */
void gui_swap_axes(simple_gui_t *gui, bool swap);