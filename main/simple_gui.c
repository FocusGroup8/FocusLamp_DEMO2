/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "simple_gui.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const char *TAG = "simple_gui";

// 8x8 pixel font for basic ASCII characters (0-127)
static const uint8_t font8x8[128][8] = {
    // Space (0x20)
    [0x20] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    // Numbers 0-9 (0x30-0x39)
    [0x30] = {0x18, 0x3C, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18}, // '0'
    [0x31] = {0x18, 0x38, 0x78, 0x18, 0x18, 0x18, 0x18, 0x7E}, // '1'
    [0x32] = {0x3C, 0x66, 0x06, 0x0C, 0x18, 0x30, 0x66, 0xFE}, // '2'
    [0x33] = {0x3C, 0x66, 0x06, 0x1C, 0x06, 0x06, 0x66, 0x3C}, // '3'
    [0x34] = {0x0C, 0x1C, 0x3C, 0x6C, 0x7E, 0x0C, 0x0C, 0x0C}, // '4'
    [0x35] = {0x7E, 0x60, 0x60, 0x3C, 0x06, 0x06, 0x66, 0x3C}, // '5'
    [0x36] = {0x3C, 0x66, 0x60, 0x7C, 0x66, 0x66, 0x66, 0x3C}, // '6'
    [0x37] = {0x7E, 0x06, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x30}, // '7'
    [0x38] = {0x3C, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x66, 0x3C}, // '8'
    [0x39] = {0x3C, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x66, 0x3C}, // '9'
    // Letters A-Z (0x41-0x5A)
    [0x41] = {0x18, 0x3C, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66}, // 'A'
    [0x42] = {0x7C, 0x66, 0x66, 0x7C, 0x66, 0x66, 0x66, 0x7C}, // 'B'
    [0x43] = {0x3C, 0x66, 0x60, 0x60, 0x60, 0x60, 0x66, 0x3C}, // 'C'
    [0x44] = {0x78, 0x6C, 0x66, 0x66, 0x66, 0x66, 0x6C, 0x78}, // 'D'
    [0x45] = {0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x60, 0x7E}, // 'E'
    [0x46] = {0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x60, 0x60}, // 'F'
    [0x47] = {0x3C, 0x66, 0x60, 0x6E, 0x66, 0x66, 0x66, 0x3C}, // 'G'
    [0x48] = {0x66, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x66}, // 'H'
    [0x49] = {0x3C, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C}, // 'I'
    [0x4A] = {0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x6C, 0x6C, 0x38}, // 'J'
    [0x4B] = {0x66, 0x6C, 0x78, 0x70, 0x78, 0x6C, 0x66, 0x66}, // 'K'
    [0x4C] = {0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7E}, // 'L'
    [0x4D] = {0x63, 0x77, 0x7F, 0x6B, 0x63, 0x63, 0x63, 0x63}, // 'M'
    [0x4E] = {0x66, 0x76, 0x7E, 0x7E, 0x6E, 0x66, 0x66, 0x66}, // 'N'
    [0x4F] = {0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C}, // 'O'
    [0x50] = {0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x60, 0x60}, // 'P'
    [0x51] = {0x3C, 0x66, 0x66, 0x66, 0x66, 0x6E, 0x3C, 0x06}, // 'Q'
    [0x52] = {0x7C, 0x66, 0x66, 0x7C, 0x6C, 0x66, 0x66, 0x66}, // 'R'
    [0x53] = {0x3C, 0x66, 0x60, 0x3C, 0x06, 0x06, 0x66, 0x3C}, // 'S'
    [0x54] = {0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18}, // 'T'
    [0x55] = {0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C}, // 'U'
    [0x56] = {0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18}, // 'V'
    [0x57] = {0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x63}, // 'W'
    [0x58] = {0x66, 0x66, 0x3C, 0x18, 0x3C, 0x66, 0x66, 0x66}, // 'X'
    [0x59] = {0x66, 0x66, 0x66, 0x3C, 0x18, 0x18, 0x18, 0x18}, // 'Y'
    [0x5A] = {0x7E, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x66, 0x7E}, // 'Z'
    // Colon (0x3A)
    [0x3A] = {0x00, 0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00}, // ':'
    // Hyphen (0x2D)
    [0x2D] = {0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00}, // '-'
};

/**
 * @brief Initialize GUI context
 */
void simple_gui_init(simple_gui_t *gui, esp_lcd_panel_handle_t panel, uint16_t width, uint16_t height)
{
    gui->panel = panel;
    gui->width = width;
    gui->height = height;
}

/**
 * @brief Clear screen to a specific color
 */
void gui_clear_screen(simple_gui_t *gui, uint32_t color)
{
    gui_draw_filled_rect(gui, 0, 0, gui->width - 1, gui->height - 1, color);
}

/**
 * @brief Draw a pixel at specified position
 */
void gui_draw_pixel(simple_gui_t *gui, int x, int y, uint32_t color)
{
    if (x < 0 || x >= gui->width || y < 0 || y >= gui->height) {
        return;
    }
    
    uint8_t pixel[3];
    pixel[0] = (color >> 16) & 0xFF; // R
    pixel[1] = (color >> 8) & 0xFF;  // G
    pixel[2] = color & 0xFF;         // B
    
    esp_lcd_panel_draw_bitmap(gui->panel, x, y, x + 1, y + 1, pixel);
}

/**
 * @brief Draw a horizontal line
 */
void gui_draw_hline(simple_gui_t *gui, int x, int y, int length, uint32_t color)
{
    if (y < 0 || y >= gui->height) {
        return;
    }
    if (x < 0) {
        length += x;
        x = 0;
    }
    if (x + length > gui->width) {
        length = gui->width - x;
    }
    if (length <= 0) {
        return;
    }
    
    size_t buffer_size = length * 3;
    uint8_t *pixel_buffer = (uint8_t *)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pixel_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate pixel buffer from PSRAM");
        return;
    }
    
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;
    
    for (int i = 0; i < length; i++) {
        pixel_buffer[i * 3 + 0] = r;
        pixel_buffer[i * 3 + 1] = g;
        pixel_buffer[i * 3 + 2] = b;
    }
    
    esp_lcd_panel_draw_bitmap(gui->panel, x, y, x + length, y + 1, pixel_buffer);
    free(pixel_buffer);
}

/**
 * @brief Draw a vertical line
 */
void gui_draw_vline(simple_gui_t *gui, int x, int y, int length, uint32_t color)
{
    if (x < 0 || x >= gui->width) {
        return;
    }
    if (y < 0) {
        length += y;
        y = 0;
    }
    if (y + length > gui->height) {
        length = gui->height - y;
    }
    if (length <= 0) {
        return;
    }
    
    size_t buffer_size = length * 3;
    uint8_t *pixel_buffer = (uint8_t *)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pixel_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate pixel buffer from PSRAM");
        return;
    }
    
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;
    
    for (int i = 0; i < length; i++) {
        pixel_buffer[i * 3 + 0] = r;
        pixel_buffer[i * 3 + 1] = g;
        pixel_buffer[i * 3 + 2] = b;
    }
    
    esp_lcd_panel_draw_bitmap(gui->panel, x, y, x + 1, y + length, pixel_buffer);
    free(pixel_buffer);
}

/**
 * @brief Draw a line between two points (Bresenham's algorithm)
 */
void gui_draw_line(simple_gui_t *gui, int x1, int y1, int x2, int y2, uint32_t color)
{
    int dx = abs(x2 - x1);
    int dy = abs(y2 - y1);
    int sx = (x1 < x2) ? 1 : -1;
    int sy = (y1 < y2) ? 1 : -1;
    int err = dx - dy;
    
    while (1) {
        gui_draw_pixel(gui, x1, y1, color);
        
        if (x1 == x2 && y1 == y2) {
            break;
        }
        
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x1 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y1 += sy;
        }
    }
}

/**
 * @brief Draw a filled rectangle
 */
void gui_draw_filled_rect(simple_gui_t *gui, int x1, int y1, int x2, int y2, uint32_t color)
{
    // Clip coordinates
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= gui->width) x2 = gui->width - 1;
    if (y2 >= gui->height) y2 = gui->height - 1;
    
    if (x1 > x2 || y1 > y2) {
        return;
    }
    
    int width = x2 - x1 + 1;
    int height = y2 - y1 + 1;
    size_t buffer_size = width * height * 3;
    
    uint8_t *pixel_buffer = (uint8_t *)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pixel_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate pixel buffer from PSRAM");
        return;
    }
    
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;
    
    for (int i = 0; i < width * height; i++) {
        pixel_buffer[i * 3 + 0] = r;
        pixel_buffer[i * 3 + 1] = g;
        pixel_buffer[i * 3 + 2] = b;
    }
    
    esp_lcd_panel_draw_bitmap(gui->panel, x1, y1, x2 + 1, y2 + 1, pixel_buffer);
    free(pixel_buffer);
}

/**
 * @brief Draw a rectangle outline
 */
void gui_draw_rect_outline(simple_gui_t *gui, int x1, int y1, int x2, int y2, uint32_t color, uint8_t thickness)
{
    for (int i = 0; i < thickness; i++) {
        gui_draw_hline(gui, x1, y1 + i, x2 - x1 + 1, color);
        gui_draw_hline(gui, x1, y2 - i, x2 - x1 + 1, color);
        gui_draw_vline(gui, x1 + i, y1, y2 - y1 + 1, color);
        gui_draw_vline(gui, x2 - i, y1, y2 - y1 + 1, color);
    }
}

/**
 * @brief Draw a filled circle (Midpoint circle algorithm)
 */
void gui_draw_filled_circle(simple_gui_t *gui, int cx, int cy, int radius, uint32_t color)
{
    if (radius <= 0) {
        return;
    }
    
    for (int y = -radius; y <= radius; y++) {
        for (int x = -radius; x <= radius; x++) {
            if (x * x + y * y <= radius * radius) {
                gui_draw_pixel(gui, cx + x, cy + y, color);
            }
        }
    }
}

/**
 * @brief Draw a circle outline
 */
void gui_draw_circle_outline(simple_gui_t *gui, int cx, int cy, int radius, uint32_t color, uint8_t thickness)
{
    if (radius <= 0) {
        return;
    }
    
    for (int t = 0; t < thickness; t++) {
        int r = radius - t;
        if (r <= 0) break;
        
        int x = r;
        int y = 0;
        int err = 0;
        
        while (x >= y) {
            gui_draw_pixel(gui, cx + x, cy + y, color);
            gui_draw_pixel(gui, cx + y, cy + x, color);
            gui_draw_pixel(gui, cx - y, cy + x, color);
            gui_draw_pixel(gui, cx - x, cy + y, color);
            gui_draw_pixel(gui, cx - x, cy - y, color);
            gui_draw_pixel(gui, cx - y, cy - x, color);
            gui_draw_pixel(gui, cx + y, cy - x, color);
            gui_draw_pixel(gui, cx + x, cy - y, color);
            
            if (err <= 0) {
                y += 1;
                err += 2 * y + 1;
            }
            if (err > 0) {
                x -= 1;
                err -= 2 * x + 1;
            }
        }
    }
}

/**
 * @brief Draw a simple 8x8 bitmap character (ASCII)
 */
void gui_draw_char(simple_gui_t *gui, int x, int y, char ch, uint32_t color, uint32_t bg_color, uint8_t size)
{
    int ch_int = (int)ch;
    if (ch_int < 0 || ch_int > 127) {
        ch = '?';
        ch_int = '?';
    }
    
    const uint8_t *bitmap = font8x8[ch_int];
    
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            // Flip horizontally: bit 7 corresponds to col 0, bit 0 corresponds to col 7
            if (bitmap[row] & (1 << (7 - col))) {
                if (size == 1) {
                    gui_draw_pixel(gui, x + col, y + row, color);
                } else {
                    gui_draw_filled_rect(gui, x + col * size, y + row * size, 
                                        x + (col + 1) * size - 1, y + (row + 1) * size - 1, color);
                }
            } else if (bg_color != COLOR_BLACK) {
                if (size == 1) {
                    gui_draw_pixel(gui, x + col, y + row, bg_color);
                } else {
                    gui_draw_filled_rect(gui, x + col * size, y + row * size, 
                                        x + (col + 1) * size - 1, y + (row + 1) * size - 1, bg_color);
                }
            }
        }
    }
}

/**
 * @brief Draw a text string
 */
void gui_draw_string(simple_gui_t *gui, int x, int y, const char *str, uint32_t color, uint32_t bg_color, uint8_t size)
{
    int cursor_x = x;
    int cursor_y = y;
    int char_width = 8 * size;
    
    while (*str) {
        gui_draw_char(gui, cursor_x, cursor_y, *str, color, bg_color, size);
        cursor_x += char_width;
        
        // Wrap to next line if exceeds screen width
        if (cursor_x >= gui->width) {
            cursor_x = x;
            cursor_y += 8 * size;
        }
        
        str++;
    }
}

/**
 * @brief Draw a number
 */
void gui_draw_number(simple_gui_t *gui, int x, int y, int num, uint32_t color, uint32_t bg_color, uint8_t size)
{
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%d", num);
    gui_draw_string(gui, x, y, buffer, color, bg_color, size);
}

/**
 * @brief Fill screen with gradient pattern
 */
void gui_draw_gradient(simple_gui_t *gui, uint32_t start_color, uint32_t end_color, bool horizontal)
{
    uint8_t start_r = (start_color >> 16) & 0xFF;
    uint8_t start_g = (start_color >> 8) & 0xFF;
    uint8_t start_b = start_color & 0xFF;
    
    uint8_t end_r = (end_color >> 16) & 0xFF;
    uint8_t end_g = (end_color >> 8) & 0xFF;
    uint8_t end_b = end_color & 0xFF;
    
    int steps = horizontal ? gui->width : gui->height;
    
    for (int i = 0; i < steps; i++) {
        float ratio = (float)i / (steps - 1);
        uint8_t r = start_r + (uint8_t)((end_r - start_r) * ratio);
        uint8_t g = start_g + (uint8_t)((end_g - start_g) * ratio);
        uint8_t b = start_b + (uint8_t)((end_b - start_b) * ratio);
        uint32_t color = (r << 16) | (g << 8) | b;
        
        if (horizontal) {
            gui_draw_vline(gui, i, 0, gui->height, color);
        } else {
            gui_draw_hline(gui, 0, i, gui->width, color);
        }
    }
}

/**
 * @brief Draw a test pattern grid
 */
void gui_draw_grid_test(simple_gui_t *gui, int grid_size)
{
    for (int x = 0; x < gui->width; x += grid_size) {
        gui_draw_vline(gui, x, 0, gui->height, COLOR_GRAY);
    }
    for (int y = 0; y < gui->height; y += grid_size) {
        gui_draw_hline(gui, 0, y, gui->width, COLOR_GRAY);
    }
}

/**
 * @brief Invert display colors
 */
void gui_invert_display(simple_gui_t *gui, bool invert)
{
    esp_lcd_panel_invert_color(gui->panel, invert);
}

/**
 * @brief Mirror display (X or Y axis)
 */
void gui_mirror_display(simple_gui_t *gui, bool mirror_x, bool mirror_y)
{
    esp_lcd_panel_mirror(gui->panel, mirror_x, mirror_y);
}

/**
 * @brief Swap display X and Y axes (rotation)
 */
void gui_swap_axes(simple_gui_t *gui, bool swap)
{
    esp_lcd_panel_swap_xy(gui->panel, swap);
}