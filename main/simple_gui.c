/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "simple_gui.h"

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "simple_gui";

/*===========================================================================*/
/* Internal helpers for double-buffer direct-fb mode                        */
/*===========================================================================*/

/**
 * @brief Get the back frame buffer pointer (the one being drawn to)
 */
static inline uint8_t *gui_back_fb(simple_gui_t *gui)
{
    return (uint8_t *)gui->fb[gui->cur_fb ^ 1];
}

/**
 * @brief Write a single pixel directly into the back frame buffer
 *
 * No bounds checking here — callers must clip. Intentionally inlined for speed.
 */
static inline void gui_fb_set_pixel(simple_gui_t *gui, int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t *p = gui_back_fb(gui) + ((size_t)y * gui->width + x) * 3;
    p[0]       = r;
    p[1]       = g;
    p[2]       = b;
}

/**
 * @brief Fill a horizontal span in the back frame buffer (32-bit optimized)
 *
 * Uses alignment-aware uint32_t writes for the bulk, falling back to byte
 * writes for unaligned head/tail. ~3x faster than byte-by-byte for long spans.
 *
 * @param x0  Start x (inclusive, clipped)
 * @param x1  End x (inclusive, clipped)
 * @param y   Row (clipped)
 */
static void gui_fb_hspan(simple_gui_t *gui, int x0, int x1, int y, uint8_t r, uint8_t g, uint8_t b)
{
    int count = x1 - x0 + 1;
    if (count <= 0)
        return;

    uint8_t *line = gui_back_fb(gui) + ((size_t)y * gui->width + x0) * 3;

    /*
     * RGB888 = 3 bytes/pixel. A 4-pixel group (12 bytes) maps to 3 uint32_t:
     *   v0 = R G B R   v1 = G B R G   v2 = B R G B
     * This pattern repeats every 4 pixels.
     */
    uint32_t v0 = ((uint32_t)r) | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)r << 24);
    uint32_t v1 = ((uint32_t)g) | ((uint32_t)b << 8) | ((uint32_t)r << 16) | ((uint32_t)g << 24);
    uint32_t v2 = ((uint32_t)b) | ((uint32_t)r << 8) | ((uint32_t)g << 16) | ((uint32_t)b << 24);

    /* Handle unaligned head: write bytes until 4-byte aligned */
    uintptr_t addr = (uintptr_t)line;
    int head       = (4 - (addr & 3)) & 3;
    head           = head / 3; /* how many whole pixels to reach alignment */
    if (head == 2)
        head = 0; /* can't align with 2 pixels (6 bytes) */
    if (head > count)
        head = 0;

    uint8_t *p = line;
    for (int i = 0; i < head; i++) {
        *p++ = r;
        *p++ = g;
        *p++ = b;
    }
    count -= head;

    /* Bulk: write 4-pixel groups (12 bytes = 3 uint32_t) */
    int groups = count / 4;
    if (groups > 0 && ((uintptr_t)p & 3) == 0) {
        uint32_t *wp = (uint32_t *)p;
        for (int i = 0; i < groups; i++) {
            *wp++ = v0;
            *wp++ = v1;
            *wp++ = v2;
        }
        p = (uint8_t *)wp;
    } else {
        /* Fallback if alignment failed: byte writes for the bulk */
        groups = 0;
    }
    count -= groups * 4;

    /* Tail: remaining 0-3 pixels */
    for (int i = 0; i < count; i++) {
        *p++ = r;
        *p++ = g;
        *p++ = b;
    }
}

/**
 * @brief Fill a rectangular region using PPA hardware accelerator
 *
 * Uses ESP32-P4's PPA engine for DMA-based memory fill. Much faster than CPU
 * for large regions (e.g., screen clear). Falls back to CPU if PPA unavailable.
 *
 * @param x0,y0  Top-left corner (inclusive)
 * @param x1,y1  Bottom-right corner (inclusive)
 */
static void gui_fb_fill_rect_ppa(simple_gui_t *gui, int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b)
{
    if (!gui->ppa_fill) {
        /* Fallback: CPU fill */
        for (int y = y0; y <= y1; y++) {
            gui_fb_hspan(gui, x0, x1, y, r, g, b);
        }
        return;
    }

    uint32_t fill_w = x1 - x0 + 1;
    uint32_t fill_h = y1 - y0 + 1;

    ppa_fill_oper_config_t config = {
        .out =
            {
                .buffer         = gui_back_fb(gui),
                .buffer_size    = gui->fb_size,
                .pic_w          = gui->width,
                .pic_h          = gui->height,
                .block_offset_x = x0,
                .block_offset_y = y0,
                .fill_cm        = PPA_FILL_COLOR_MODE_RGB888,
            },
        .fill_block_w = fill_w,
        .fill_block_h = fill_h,
        .fill_argb_color =
            {
                .val = (0xFFu << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b,
            },
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    esp_err_t ret = ppa_do_fill(gui->ppa_fill, &config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "PPA fill failed (%s), falling back to CPU", esp_err_to_name(ret));
        for (int y = y0; y <= y1; y++) {
            gui_fb_hspan(gui, x0, x1, y, r, g, b);
        }
    }
}

// 8x8 pixel font for basic ASCII characters (0-127)
// Each character is 8 bytes, each byte represents a row (bit 7 = leftmost pixel)
static const uint8_t font8x8[128][8] = {
    // Space (0x20)
    [0x20] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    // Punctuation
    [0x21] = {0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x18, 0x00}, // '!'
    [0x22] = {0x66, 0x66, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00}, // '"'
    [0x23] = {0x66, 0x66, 0x7E, 0x66, 0x7E, 0x66, 0x66, 0x00}, // '#'
    [0x24] = {0x18, 0x3C, 0x60, 0x3C, 0x06, 0x3C, 0x18, 0x00}, // '$'
    [0x25] = {0x62, 0x66, 0x0C, 0x18, 0x30, 0x66, 0x46, 0x00}, // '%'
    [0x26] = {0x3C, 0x66, 0x3C, 0x38, 0x67, 0x66, 0x3F, 0x00}, // '&'
    [0x27] = {0x18, 0x18, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00}, // '\''
    [0x28] = {0x0E, 0x18, 0x30, 0x30, 0x30, 0x18, 0x0E, 0x00}, // '('
    [0x29] = {0x70, 0x18, 0x0C, 0x0C, 0x0C, 0x18, 0x70, 0x00}, // ')'
    [0x2A] = {0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00}, // '*'
    [0x2B] = {0x00, 0x18, 0x18, 0x7E, 0x18, 0x18, 0x00, 0x00}, // '+'
    [0x2C] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x30}, // ','
    // Hyphen (0x2D)
    [0x2D] = {0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00}, // '-'
    [0x2E] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00}, // '.'
    [0x2F] = {0x00, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x00, 0x00}, // '/'
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
    // More punctuation
    // Colon (0x3A)
    [0x3A] = {0x00, 0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00}, // ':'
    [0x3B] = {0x00, 0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x30}, // ';'
    [0x3C] = {0x00, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x00, 0x00}, // '<'
    [0x3D] = {0x00, 0x00, 0x7E, 0x00, 0x7E, 0x00, 0x00, 0x00}, // '='
    [0x3E] = {0x00, 0x60, 0x30, 0x18, 0x0C, 0x06, 0x00, 0x00}, // '>'
    [0x3F] = {0x3C, 0x66, 0x06, 0x0C, 0x18, 0x00, 0x18, 0x00}, // '?'
    [0x40] = {0x3C, 0x66, 0x6E, 0x6E, 0x60, 0x62, 0x3C, 0x00}, // '@'
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
    // More punctuation
    [0x5B] = {0x1E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1E}, // '['
    [0x5C] = {0x00, 0x60, 0x30, 0x18, 0x0C, 0x06, 0x00, 0x00}, // '\'
    [0x5D] = {0x78, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x78}, // ']'
    [0x5E] = {0x08, 0x1C, 0x36, 0x63, 0x00, 0x00, 0x00, 0x00}, // '^'
    [0x5F] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF}, // '_'
    [0x60] = {0x18, 0x18, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00}, // '`'
    // Letters a-z (0x61-0x7A)
    [0x61] = {0x00, 0x00, 0x3C, 0x06, 0x3E, 0x66, 0x3E, 0x00}, // 'a'
    [0x62] = {0x60, 0x60, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x7C}, // 'b'
    [0x63] = {0x00, 0x00, 0x3C, 0x66, 0x60, 0x66, 0x3C, 0x00}, // 'c'
    [0x64] = {0x06, 0x06, 0x3E, 0x66, 0x66, 0x66, 0x66, 0x3E}, // 'd'
    [0x65] = {0x00, 0x00, 0x3C, 0x66, 0x7E, 0x60, 0x3C, 0x00}, // 'e'
    [0x66] = {0x0C, 0x18, 0x18, 0x3C, 0x18, 0x18, 0x18, 0x18}, // 'f'
    [0x67] = {0x00, 0x00, 0x3E, 0x66, 0x66, 0x3E, 0x06, 0x3C}, // 'g'
    [0x68] = {0x60, 0x60, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x66}, // 'h'
    [0x69] = {0x18, 0x00, 0x38, 0x18, 0x18, 0x18, 0x18, 0x3C}, // 'i'
    [0x6A] = {0x0C, 0x00, 0x1C, 0x0C, 0x0C, 0x0C, 0x0C, 0x78}, // 'j'
    [0x6B] = {0x60, 0x60, 0x66, 0x6C, 0x78, 0x6C, 0x66, 0x66}, // 'k'
    [0x6C] = {0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C}, // 'l'
    [0x6D] = {0x00, 0x00, 0x66, 0x7F, 0x7F, 0x6B, 0x63, 0x63}, // 'm'
    [0x6E] = {0x00, 0x00, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x66}, // 'n'
    [0x6F] = {0x00, 0x00, 0x3C, 0x66, 0x66, 0x66, 0x66, 0x3C}, // 'o'
    [0x70] = {0x00, 0x00, 0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60}, // 'p'
    [0x71] = {0x00, 0x00, 0x3E, 0x66, 0x66, 0x3E, 0x06, 0x06}, // 'q'
    [0x72] = {0x00, 0x00, 0x7C, 0x66, 0x60, 0x60, 0x60, 0x60}, // 'r'
    [0x73] = {0x00, 0x00, 0x3E, 0x60, 0x3C, 0x06, 0x7C, 0x00}, // 's'
    [0x74] = {0x18, 0x18, 0x7E, 0x18, 0x18, 0x18, 0x18, 0x0C}, // 't'
    [0x75] = {0x00, 0x00, 0x66, 0x66, 0x66, 0x66, 0x3E, 0x00}, // 'u'
    [0x76] = {0x00, 0x00, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00}, // 'v'
    [0x77] = {0x00, 0x00, 0x63, 0x6B, 0x7F, 0x7F, 0x36, 0x00}, // 'w'
    [0x78] = {0x00, 0x00, 0x66, 0x3C, 0x18, 0x3C, 0x66, 0x00}, // 'x'
    [0x79] = {0x00, 0x00, 0x66, 0x66, 0x66, 0x3E, 0x06, 0x3C}, // 'y'
    [0x7A] = {0x00, 0x00, 0x7E, 0x0C, 0x18, 0x30, 0x7E, 0x00}, // 'z'
    [0x7B] = {0x0E, 0x18, 0x18, 0x70, 0x18, 0x18, 0x0E, 0x00}, // '{'
    [0x7C] = {0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18}, // '|'
    [0x7D] = {0x70, 0x18, 0x18, 0x0E, 0x18, 0x18, 0x70, 0x00}, // '}'
    [0x7E] = {0x3C, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // '~'
};

/**
 * @brief Initialize GUI context (single-buffer legacy mode)
 */
void simple_gui_init(simple_gui_t *gui, esp_lcd_panel_handle_t panel, uint16_t width, uint16_t height)
{
    gui->panel           = panel;
    gui->width           = width;
    gui->height          = height;
    gui->double_buffered = false;
    gui->fb[0]           = NULL;
    gui->fb[1]           = NULL;
    gui->cur_fb          = 0;
    gui->fb_size         = 0;
    gui->bpp             = 24;
    gui->ppa_fill        = NULL;
}

/**
 * @brief Initialize GUI with double-buffer support (direct-fb mode)
 */
void gui_init_double_buffer(simple_gui_t *gui, esp_lcd_panel_handle_t panel, uint16_t width, uint16_t height)
{
    simple_gui_init(gui, panel, width, height);

    void *fb0 = NULL;
    void *fb1 = NULL;
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(panel, 2, &fb0, &fb1));
    gui->fb[0]           = fb0;
    gui->fb[1]           = fb1;
    gui->cur_fb          = 0; // fb[0] is displayed first; we draw to fb[1]
    gui->bpp             = 24;
    gui->fb_size         = (size_t)width * height * (gui->bpp / 8);
    gui->double_buffered = true;

    // Register PPA fill client for hardware-accelerated fills
    ppa_client_config_t ppa_cfg = {
        .oper_type = PPA_OPERATION_FILL,
    };
    esp_err_t ret = ppa_register_client(&ppa_cfg, &gui->ppa_fill);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "PPA fill client registered (hardware acceleration enabled)");
    } else {
        ESP_LOGW(TAG, "PPA register failed (%s), using CPU fill", esp_err_to_name(ret));
        gui->ppa_fill = NULL;
    }

    // Clear both frame buffers to black
    memset(fb0, 0, gui->fb_size);
    memset(fb1, 0, gui->fb_size);
    esp_cache_msync(fb0, gui->fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    esp_cache_msync(fb1, gui->fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

    ESP_LOGI(TAG, "Double-buffer mode: fb0=%p fb1=%p size=%u", fb0, fb1, (unsigned)gui->fb_size);
}

/**
 * @brief Swap front/back frame buffers (present the drawn frame)
 */
void gui_swap_buffers(simple_gui_t *gui)
{
    if (!gui->double_buffered) {
        return;
    }
    // The back buffer is fb[cur_fb ^ 1]. Flush its cache so DMA sees latest data,
    // then call draw_bitmap with that pointer — the DPI driver detects the pointer
    // is within fb memory range and switches cur_fb_index (zero-copy flip).
    uint8_t *back = gui_back_fb(gui);
    esp_cache_msync(back, gui->fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    esp_lcd_panel_draw_bitmap(gui->panel, 0, 0, gui->width, gui->height, back);
    gui->cur_fb ^= 1;
}

/**
 * @brief Clear screen to a specific color
 */
void gui_clear_screen(simple_gui_t *gui, uint32_t color)
{
    if (gui->double_buffered) {
        uint8_t r = (color >> 16) & 0xFF;
        uint8_t g = (color >> 8) & 0xFF;
        uint8_t b = color & 0xFF;
        /* Full-screen fill: use PPA hardware accelerator */
        gui_fb_fill_rect_ppa(gui, 0, 0, gui->width - 1, gui->height - 1, r, g, b);
        return;
    }
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
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;

    if (gui->double_buffered) {
        gui_fb_set_pixel(gui, x, y, r, g, b);
        return;
    }

    uint8_t pixel[3] = {r, g, b};
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
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;

    if (gui->double_buffered) {
        gui_fb_hspan(gui, x, x + length - 1, y, r, g, b);
        return;
    }

    size_t buffer_size    = length * 3;
    uint8_t *pixel_buffer = (uint8_t *)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pixel_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate pixel buffer from PSRAM");
        return;
    }
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
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;

    if (gui->double_buffered) {
        for (int i = 0; i < length; i++) {
            gui_fb_set_pixel(gui, x, y + i, r, g, b);
        }
        return;
    }

    size_t buffer_size    = length * 3;
    uint8_t *pixel_buffer = (uint8_t *)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pixel_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate pixel buffer from PSRAM");
        return;
    }
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
    int dx  = abs(x2 - x1);
    int dy  = abs(y2 - y1);
    int sx  = (x1 < x2) ? 1 : -1;
    int sy  = (y1 < y2) ? 1 : -1;
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
    if (x1 < 0)
        x1 = 0;
    if (y1 < 0)
        y1 = 0;
    if (x2 >= gui->width)
        x2 = gui->width - 1;
    if (y2 >= gui->height)
        y2 = gui->height - 1;

    if (x1 > x2 || y1 > y2) {
        return;
    }
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;

    if (gui->double_buffered) {
        /* Use PPA hardware accelerator for filled rectangles */
        gui_fb_fill_rect_ppa(gui, x1, y1, x2, y2, r, g, b);
        return;
    }

    int width             = x2 - x1 + 1;
    int height            = y2 - y1 + 1;
    size_t buffer_size    = width * height * 3;
    uint8_t *pixel_buffer = (uint8_t *)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pixel_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate pixel buffer from PSRAM");
        return;
    }
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
 * @brief Draw a filled circle (scanline-rendered for performance)
 */
void gui_draw_filled_circle(simple_gui_t *gui, int cx, int cy, int radius, uint32_t color)
{
    if (radius <= 0) {
        return;
    }

    // Fast path for single-pixel radius
    if (radius == 1) {
        gui_draw_pixel(gui, cx, cy, color);
        return;
    }

    const uint8_t r = (color >> 16) & 0xFF;
    const uint8_t g = (color >> 8) & 0xFF;
    const uint8_t b = color & 0xFF;

    if (gui->double_buffered) {
        // Direct-fb path: write spans straight into the back buffer
        for (int y = -radius; y <= radius; y++) {
            const int dy = cy + y;
            if (dy < 0 || dy >= gui->height) {
                continue;
            }
            const int rad_sq_minus_y_sq = radius * radius - y * y;
            if (rad_sq_minus_y_sq < 0) {
                continue;
            }
            const int half = (int)sqrtf((float)rad_sq_minus_y_sq);
            int x0         = cx - half;
            int x1         = cx + half;
            if (x1 < 0 || x0 >= gui->width) {
                continue;
            }
            if (x0 < 0)
                x0 = 0;
            if (x1 >= gui->width)
                x1 = gui->width - 1;
            gui_fb_hspan(gui, x0, x1, dy, r, g, b);
        }
        return;
    }

    // Legacy single-buffer path: batch with a line buffer + draw_bitmap
    const int max_w   = (2 * radius + 1);
    uint8_t *line_buf = (uint8_t *)malloc(max_w * 3);
    if (line_buf == NULL) {
        // Fallback: per-pixel path (slow but correct)
        for (int y = -radius; y <= radius; y++) {
            int dx = (int)(radius * radius - y * y);
            if (dx < 0)
                continue;
            int half = (int)sqrtf((float)dx);
            for (int x = -half; x <= half; x++) {
                gui_draw_pixel(gui, cx + x, cy + y, color);
            }
        }
        return;
    }

    // Pre-fill the buffer with the solid color once; we'll reuse it for every scanline.
    for (int i = 0; i < max_w * 3; i += 3) {
        line_buf[i]     = r;
        line_buf[i + 1] = g;
        line_buf[i + 2] = b;
    }

    // Scanline fill: for each y, compute the half-width via sqrt.
    for (int y = -radius; y <= radius; y++) {
        const int dy = cy + y;
        if (dy < 0 || dy >= gui->height) {
            continue;
        }
        const int rad_sq_minus_y_sq = radius * radius - y * y;
        if (rad_sq_minus_y_sq < 0) {
            continue;
        }
        const int half = (int)sqrtf((float)rad_sq_minus_y_sq);
        int x0         = cx - half;
        int x1         = cx + half;
        if (x1 < 0 || x0 >= gui->width) {
            continue;
        }
        // Clip x range
        int clip_left = 0;
        if (x0 < 0) {
            clip_left = -x0;
            x0        = 0;
        }
        if (x1 >= gui->width) {
            x1 = gui->width - 1;
        }
        esp_lcd_panel_draw_bitmap(gui->panel, x0, dy, x1 + 1, dy + 1, &line_buf[clip_left * 3]);
    }

    free(line_buf);
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
        if (r <= 0)
            break;

        int x   = r;
        int y   = 0;
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
 * @brief Draw a filled pie slice (sector)
 *
 * Normalizes angles to [0, 2π) and fills pixels whose angle from center
 * falls within [start, start+slice). Uses direct fb writes in double-buffer
 * mode for speed.
 */
void gui_draw_filled_pie(simple_gui_t *gui, int cx, int cy, int radius, float start_angle, float slice_angle,
                         uint32_t color)
{
    if (radius <= 0 || slice_angle <= 0) {
        return;
    }

    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;

    /* Normalize start to [0, 2π) */
    float two_pi = 2.0f * M_PI;
    while (start_angle < 0)
        start_angle += two_pi;
    while (start_angle >= two_pi)
        start_angle -= two_pi;

    float end_angle = start_angle + slice_angle;
    /* end_angle may exceed 2π; handle wrap-around in the pixel test */

    for (int y = -radius; y <= radius; y++) {
        int py = cy + y;
        if (py < 0 || py >= gui->height)
            continue;

        int dy_sq  = y * y;
        int max_dx = (int)sqrtf((float)(radius * radius - dy_sq));
        if (max_dx <= 0)
            continue;

        for (int x = -max_dx; x <= max_dx; x++) {
            int px = cx + x;
            if (px < 0 || px >= gui->width)
                continue;

            /* Angle of this pixel relative to center */
            float angle = atan2f((float)y, (float)x);
            if (angle < 0)
                angle += two_pi;

            /* Check if angle is within the slice (handle wrap) */
            bool in_slice;
            if (end_angle <= two_pi) {
                in_slice = (angle >= start_angle && angle < end_angle);
            } else {
                /* Wrap: [start, 2π) ∪ [0, end-2π) */
                in_slice = (angle >= start_angle || angle < (end_angle - two_pi));
            }

            if (in_slice) {
                if (gui->double_buffered) {
                    gui_fb_set_pixel(gui, px, py, r, g, b);
                } else {
                    gui_draw_pixel(gui, px, py, color);
                }
            }
        }
    }
}

/**
 * @brief Draw a simple 8x8 bitmap character (ASCII)
 *
 * Optimized: builds the whole glyph in a single line buffer and pushes it
 * to the panel with one esp_lcd_panel_draw_bitmap call, eliminating
 * per-pixel DSI traffic. Also fixes the text-overlap bug by honoring
 * COLOR_TRANSPARENT: when bg_color == COLOR_TRANSPARENT, background pixels
 * are skipped (no clear), so text can be drawn on top of existing content.
 *
 * @param size     Scaling factor (1..4). Larger sizes use a 32x32 max buffer.
 * @param bg_color Background color, or COLOR_TRANSPARENT to skip background.
 */
void gui_draw_char(simple_gui_t *gui, int x, int y, char ch, uint32_t color, uint32_t bg_color, uint8_t size)
{
    if (size == 0) {
        size = 1;
    }
    if (size > 4) {
        size = 4; // Clamp to limit buffer size (32x32x3 = 3072 bytes)
    }

    int ch_int = (int)(uint8_t)ch;
    if (ch_int > 127) {
        ch_int = '?';
    }

    const uint8_t *bitmap = font8x8[ch_int];
    const int glyph_w     = 8 * size;
    const int glyph_h     = 8 * size;

    // Clip check: skip entirely if outside visible area
    if (x >= gui->width || y >= gui->height || x + glyph_w <= 0 || y + glyph_h <= 0) {
        return;
    }

    // Single-pixel color components
    const uint8_t r = (color >> 16) & 0xFF;
    const uint8_t g = (color >> 8) & 0xFF;
    const uint8_t b = color & 0xFF;

    const bool transparent_bg = (bg_color == COLOR_TRANSPARENT);

    /*---- Double-buffer direct-fb path ----*/
    if (gui->double_buffered) {
        if (!transparent_bg) {
            const uint8_t bg_r = (bg_color >> 16) & 0xFF;
            const uint8_t bg_g = (bg_color >> 8) & 0xFF;
            const uint8_t bg_b = bg_color & 0xFF;
            for (int row = 0; row < 8; row++) {
                for (int srow = 0; srow < size; srow++) {
                    const int dy = y + row * size + srow;
                    if (dy < 0 || dy >= gui->height)
                        continue;
                    for (int col = 0; col < 8; col++) {
                        const bool on    = (bitmap[row] & (1 << (7 - col))) != 0;
                        const uint8_t pr = on ? r : bg_r;
                        const uint8_t pg = on ? g : bg_g;
                        const uint8_t pb = on ? b : bg_b;
                        int x0           = x + col * size;
                        int x1           = x0 + size - 1;
                        if (x1 < 0 || x0 >= gui->width)
                            continue;
                        if (x0 < 0)
                            x0 = 0;
                        if (x1 >= gui->width)
                            x1 = gui->width - 1;
                        gui_fb_hspan(gui, x0, x1, dy, pr, pg, pb);
                    }
                }
            }
        } else {
            // Transparent bg: only write foreground runs
            for (int row = 0; row < 8; row++) {
                for (int srow = 0; srow < size; srow++) {
                    const int dy = y + row * size + srow;
                    if (dy < 0 || dy >= gui->height)
                        continue;
                    int col = 0;
                    while (col < 8) {
                        if (!(bitmap[row] & (1 << (7 - col)))) {
                            col++;
                            continue;
                        }
                        int run_end = col;
                        while (run_end < 8 && (bitmap[row] & (1 << (7 - run_end))))
                            run_end++;
                        int x0 = x + col * size;
                        int x1 = x + run_end * size - 1;
                        if (x1 >= 0 && x0 < gui->width) {
                            if (x0 < 0)
                                x0 = 0;
                            if (x1 >= gui->width)
                                x1 = gui->width - 1;
                            gui_fb_hspan(gui, x0, x1, dy, r, g, b);
                        }
                        col = run_end;
                    }
                }
            }
        }
        return;
    }

    /*---- Legacy single-buffer path ----*/
    if (!transparent_bg) {
        // Opaque background path: build the whole glyph in a single buffer,
        // then flush with ONE esp_lcd_panel_draw_bitmap call.
        // Buffer sizing: size=1 uses stack (192B), size>=2 uses heap to avoid
        // stack overflow (main task stack is only ~4KB).
        const uint8_t bg_r    = (bg_color >> 16) & 0xFF;
        const uint8_t bg_g    = (bg_color >> 8) & 0xFF;
        const uint8_t bg_b    = bg_color & 0xFF;
        const int buf_stride  = glyph_w * 3;
        const size_t buf_size = (size_t)glyph_w * glyph_h * 3;

        uint8_t stack_buf[8 * 8 * 3]; // only for size==1
        uint8_t *buf;
        bool used_heap = false;

        if (size == 1) {
            buf = stack_buf;
        } else {
            buf = (uint8_t *)malloc(buf_size);
            if (buf == NULL) {
                // Fallback: per-pixel path (slow but correct)
                for (int row = 0; row < 8; row++) {
                    for (int col = 0; col < 8; col++) {
                        const bool on = (bitmap[row] & (1 << (7 - col))) != 0;
                        if (!on)
                            continue;
                        if (size == 1) {
                            gui_draw_pixel(gui, x + col, y + row, color);
                        } else {
                            gui_draw_filled_rect(gui, x + col * size, y + row * size, x + (col + 1) * size - 1,
                                                 y + (row + 1) * size - 1, color);
                        }
                    }
                }
                return;
            }
            used_heap = true;
        }

        for (int row = 0; row < 8; row++) {
            for (int srow = 0; srow < size; srow++) {
                const int dy   = row * size + srow;
                uint8_t *pline = &buf[dy * buf_stride];
                for (int col = 0; col < 8; col++) {
                    const bool on    = (bitmap[row] & (1 << (7 - col))) != 0;
                    const uint8_t pr = on ? r : bg_r;
                    const uint8_t pg = on ? g : bg_g;
                    const uint8_t pb = on ? b : bg_b;
                    for (int scol = 0; scol < size; scol++) {
                        const int dx  = (col * size + scol) * 3;
                        pline[dx]     = pr;
                        pline[dx + 1] = pg;
                        pline[dx + 2] = pb;
                    }
                }
            }
        }
        esp_lcd_panel_draw_bitmap(gui->panel, x, y, x + glyph_w, y + glyph_h, buf);

        if (used_heap) {
            free(buf);
        }
        return;
    }

    // Transparent background path: we cannot use a single contiguous bitmap because
    // background pixels must not overwrite existing content. Instead, push each
    // foreground run as a small horizontal bitmap (one DSI call per run).
    // This is still far faster than per-pixel calls because most rows have few runs.
    uint8_t run_buf[32 * 3]; // max glyph_w at size=4
    for (int row = 0; row < 8; row++) {
        int col = 0;
        while (col < 8) {
            if (!(bitmap[row] & (1 << (7 - col)))) {
                col++;
                continue;
            }
            // Start of a foreground run
            int run_end = col;
            while (run_end < 8 && (bitmap[row] & (1 << (7 - run_end)))) {
                run_end++;
            }
            const int run_len = run_end - col;
            const int run_w   = run_len * size;
            // Fill run buffer with foreground color
            for (int i = 0; i < run_w * 3; i += 3) {
                run_buf[i]     = r;
                run_buf[i + 1] = g;
                run_buf[i + 2] = b;
            }
            // For each scaled row, push the run
            for (int srow = 0; srow < size; srow++) {
                const int dy = y + row * size + srow;
                if (dy < 0 || dy >= gui->height) {
                    continue;
                }
                int dx0 = x + col * size;
                int dx1 = dx0 + run_w; // exclusive
                // Skip runs entirely off-screen
                if (dx1 <= 0 || dx0 >= gui->width) {
                    continue;
                }
                // Clip x range and compute buffer offset
                int clip_left = 0;
                if (dx0 < 0) {
                    clip_left = -dx0;
                    dx0       = 0;
                }
                if (dx1 > gui->width) {
                    dx1 = gui->width;
                }
                esp_lcd_panel_draw_bitmap(gui->panel, dx0, dy, dx1, dy + 1, &run_buf[clip_left * 3]);
            }
            col = run_end;
        }
    }
}

/**
 * @brief Draw a text string
 */
void gui_draw_string(simple_gui_t *gui, int x, int y, const char *str, uint32_t color, uint32_t bg_color, uint8_t size)
{
    gui_draw_string_spacing(gui, x, y, str, color, bg_color, size, 0);
}

/**
 * @brief Draw a text string with custom character spacing
 *
 * @param spacing Extra pixels between characters (0 = characters touch, 2 = default readable spacing)
 */
void gui_draw_string_spacing(simple_gui_t *gui, int x, int y, const char *str, uint32_t color, uint32_t bg_color,
                             uint8_t size, int spacing)
{
    int cursor_x     = x;
    int cursor_y     = y;
    const int char_w = 8 * size;
    const int line_h = 8 * size;
    // Effective advance = char width + spacing; if transparent bg, skip clearing spacing column
    const int advance = char_w + spacing;

    while (*str) {
        // Handle a few control chars for minimal layout control
        if (*str == '\n') {
            cursor_x = x;
            cursor_y += line_h;
            str++;
            continue;
        }
        if (*str == '\r') {
            cursor_x = x;
            str++;
            continue;
        }

        // If bg is transparent and a spacing gap is requested, we must NOT
        // clear the gap region. gui_draw_char with COLOR_TRANSPARENT already
        // honors this, so we just advance the cursor.
        gui_draw_char(gui, cursor_x, cursor_y, *str, color, bg_color, size);
        cursor_x += advance;

        // Wrap to next line if exceeds screen width
        if (cursor_x + char_w > gui->width) {
            cursor_x = x;
            cursor_y += line_h;
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
        float ratio    = (float)i / (steps - 1);
        uint8_t r      = start_r + (uint8_t)((end_r - start_r) * ratio);
        uint8_t g      = start_g + (uint8_t)((end_g - start_g) * ratio);
        uint8_t b      = start_b + (uint8_t)((end_b - start_b) * ratio);
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