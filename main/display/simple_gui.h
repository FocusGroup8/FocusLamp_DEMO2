/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "board_config.h"
#include "driver/ppa.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"

#include <stdint.h>

/**
 * @brief Simple GUI drawing library for LCD panels
 *
 * This library provides basic drawing functions without LVGL dependency.
 * Supports RGB888 color format.
 *
 * Color constants (COLOR_RED, COLOR_WHITE, etc.) are defined in board_config.h.
 */

/**
 * @brief Special color value for transparent background.
 *        When used as bg_color in text drawing, background pixels are skipped.
 */
#define COLOR_TRANSPARENT 0x80000000U

/**
 * @brief GUI context with optional double-buffer support.
 *
 * When double_buffered is true, all drawing operations write directly to the
 * back frame buffer (fb[cur_fb ^ 1]) in PSRAM. Call gui_swap_buffers() to
 * flip the back buffer to the display, eliminating tearing/flicker.
 *
 * When double_buffered is false, drawing falls back to esp_lcd_panel_draw_bitmap
 * (legacy single-buffer mode).
 */
typedef struct {
    esp_lcd_panel_handle_t panel;
    uint16_t width;
    uint16_t height;
    /* Double-buffer fields */
    bool double_buffered; /*!< True if operating in direct-fb double-buffer mode */
    void *fb[2];          /*!< Frame buffer pointers (fb[0], fb[1]) from DPI panel */
    uint8_t cur_fb;       /*!< Index of the currently-displayed fb (0 or 1) */
    size_t fb_size;       /*!< Total size of one frame buffer in bytes */
    uint8_t bpp;          /*!< Bits per pixel (24 for RGB888) */
    /* PPA hardware accelerator */
    ppa_client_handle_t ppa_fill; /*!< PPA fill client for hardware-accelerated fills */
} simple_gui_t;

/**
 * @brief Initialize GUI context (single-buffer legacy mode)
 */
void simple_gui_init(simple_gui_t *gui, esp_lcd_panel_handle_t panel, uint16_t width, uint16_t height);

/**
 * @brief Initialize GUI with double-buffer support (direct-fb mode)
 *
 * Fetches two frame buffer pointers from the DPI panel driver and enables
 * direct-write drawing. After drawing a complete frame, call gui_swap_buffers()
 * to present it on screen.
 *
 * @note Requires BOARD_DPI_FB_COUNT >= 2 in board_config.h
 */
void gui_init_double_buffer(simple_gui_t *gui, esp_lcd_panel_handle_t panel, uint16_t width, uint16_t height);

/**
 * @brief Swap front/back frame buffers (present the drawn frame)
 *
 * In double-buffer mode, flushes the back buffer's cache and flips it to the
 * display via the DPI panel's zero-copy path. In single-buffer mode this is a no-op.
 */
void gui_swap_buffers(simple_gui_t *gui);

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
 * @brief Draw a filled pie slice (sector) from a circle
 *
 * Useful for visual rotation indicators. The slice spans from start_angle to
 * start_angle + slice_angle, measured in radians clockwise from the positive
 * X-axis (standard math convention).
 *
 * @param cx,cy        Center of the circle
 * @param radius       Circle radius
 * @param start_angle  Starting angle in radians
 * @param slice_angle  Angular width of the slice in radians
 * @param color        Fill color (RGB888)
 */
void gui_draw_filled_pie(simple_gui_t *gui, int cx, int cy, int radius, float start_angle, float slice_angle,
                         uint32_t color);

/**
 * @brief Draw a simple 8x8 bitmap character (ASCII)
 */
void gui_draw_char(simple_gui_t *gui, int x, int y, char ch, uint32_t color, uint32_t bg_color, uint8_t size);

/**
 * @brief Draw a text string
 */
void gui_draw_string(simple_gui_t *gui, int x, int y, const char *str, uint32_t color, uint32_t bg_color, uint8_t size);

/**
 * @brief Draw a text string with custom character spacing
 */
void gui_draw_string_spacing(simple_gui_t *gui, int x, int y, const char *str, uint32_t color, uint32_t bg_color,
                             uint8_t size, int spacing);

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