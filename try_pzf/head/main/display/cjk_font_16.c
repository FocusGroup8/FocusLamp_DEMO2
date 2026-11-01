/* Renderer: SPDX-License-Identifier: Apache-2.0
 * Generated font data: SIL Open Font License 1.1; see tools/fonts. */
#include "cjk_font_16.h"

#include <limits.h>
#include <stdbool.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif

#include "cjk_font_16_data.inc"

/* Invalid UTF-8 consumes one byte and renders a visible missing-glyph box. */
static uint32_t next_codepoint(const char **cursor)
{
    const unsigned char *p = (const unsigned char *)*cursor;
    unsigned count;
    uint32_t cp;
    if (!*p) return 0;
    if (*p < 0x80) { *cursor += 1; return *p; }
    if (*p >= 0xC2 && *p <= 0xDF) { count = 2; cp = *p & 0x1F; }
    else if (*p >= 0xE0 && *p <= 0xEF) { count = 3; cp = *p & 0x0F; }
    else if (*p >= 0xF0 && *p <= 0xF4) { count = 4; cp = *p & 0x07; }
    else { *cursor += 1; return 0xFFFD; }
    for (unsigned i = 1; i < count; ++i) {
        if (!p[i] || (p[i] & 0xC0) != 0x80) { *cursor += 1; return 0xFFFD; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    if ((count == 3 && cp < 0x800) || (count == 4 && cp < 0x10000) ||
        (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
        *cursor += 1; return 0xFFFD;
    }
    *cursor += count;
    return cp;
}

const uint16_t *cjk_font_16_glyph(uint32_t codepoint)
{
    size_t low = 0, high = sizeof(s_cjk16_codepoints) / sizeof(s_cjk16_codepoints[0]);
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (s_cjk16_codepoints[mid] < codepoint) low = mid + 1;
        else high = mid;
    }
    if (low < sizeof(s_cjk16_codepoints) / sizeof(s_cjk16_codepoints[0]) &&
        s_cjk16_codepoints[low] == codepoint) return s_cjk16_rows[low];
    return NULL;
}

int cjk_font_16_advance(uint32_t codepoint) { return codepoint < 0x80 ? 8 : 16; }

size_t cjk_font_16_count(const char *utf8)
{
    size_t count = 0;
    if (utf8) while (*utf8) { next_codepoint(&utf8); ++count; }
    return count;
}

int cjk_font_16_measure(const char *utf8)
{
    int line = 0, max = 0;
    if (!utf8) return 0;
    while (*utf8) {
        uint32_t cp = next_codepoint(&utf8);
        if (cp == '\n') { if (line > max) max = line; line = 0; }
        else {
            int advance = cjk_font_16_advance(cp);
            if (line > INT_MAX - advance) return INT_MAX;
            line += advance;
        }
    }
    return line > max ? line : max;
}

static bool valid_surface(uint8_t *bytes, int width, int height, int stride)
{
    return bytes && width > 0 && height > 0 && width <= INT_MAX / 2 && stride >= width * 2;
}

static void draw_glyph(uint8_t *bytes, int width, int height, int stride,
                       int x, int y, uint32_t cp, uint16_t color)
{
    if (x >= width || y >= height || x <= -16 || y <= -16) return;
    const uint16_t *rows = cjk_font_16_glyph(cp);
#ifdef ESP_PLATFORM
    /* At most 16 diagnostics per boot, with consecutive repeats suppressed. */
    static uint32_t last_missing = 0;
    static unsigned missing_logs = 0;
    if (!rows && cp != last_missing && missing_logs < 16) {
        ESP_LOGW("CJK_FONT", "Missing glyph U+%04lX", (unsigned long)cp);
        last_missing = cp;
        ++missing_logs;
    }
#endif
    int glyph_width = cjk_font_16_advance(cp);
    for (int row = 0; row < 16; ++row) {
        int dy = y + row;
        if (dy < 0 || dy >= height) continue;
        uint16_t bits = rows ? rows[row] :
            (row == 1 || row == 14 ? 0x7FFE : (row > 1 && row < 14 ? 0x4002 : 0));
        int left = x < 0 ? -x : 0;
        int right = glyph_width;
        if (right > width - x) right = width - x;
        /* Expand each bitmap row directly into the canvas. No per-pixel GUI
         * draw calls, intermediate framebuffer or heap allocation. */
        uint8_t *out = bytes + (size_t)dy * (size_t)stride + (size_t)(x + left) * 2;
        for (int col = left; col < right; ++col, out += 2) {
            if ((bits & (0x8000u >> col)) != 0) {
                out[0] = (uint8_t)color;
                out[1] = (uint8_t)(color >> 8);
            }
        }
    }
}

void cjk_font_16_draw(uint8_t *rgb565, int width, int height, int stride,
                      int x, int y, const char *utf8, uint16_t color)
{
    if (!utf8 || !valid_surface(rgb565, width, height, stride)) return;
    int offset = 0, line = 0;
    while (*utf8) {
        uint32_t cp = next_codepoint(&utf8);
        if (cp == '\n') { offset = 0; line += 16; continue; }
        /* Coordinates arrive from our bounded layout; avoid integer overflow
         * even if a caller passes a pathological off-screen origin. */
        int64_t dx = (int64_t)x + offset, dy = (int64_t)y + line;
        if (dx >= INT_MIN && dx <= INT_MAX && dy >= INT_MIN && dy <= INT_MAX)
            draw_glyph(rgb565, width, height, stride, (int)dx, (int)dy, cp, color);
        if (offset > INT_MAX - 16) return;
        offset += cjk_font_16_advance(cp);
    }
}

int cjk_font_16_draw_wrapped(uint8_t *rgb565, int width, int height, int stride,
                            int x, int y, const char *utf8, uint16_t color,
                            int max_width, int line_height, int max_lines)
{
    if (!utf8 || !*utf8 || !valid_surface(rgb565, width, height, stride) ||
        max_width < 16 || line_height < 16 || max_lines < 1 ||
        max_lines > INT_MAX / line_height) return 0;
    int offset = 0, line = 0;
    while (*utf8) {
        uint32_t cp = next_codepoint(&utf8);
        int advance = cjk_font_16_advance(cp);
        if (cp == '\n' || offset > max_width - advance) {
            if (++line >= max_lines) return max_lines * line_height;
            offset = 0;
            if (cp == '\n') continue;
        }
        int64_t dx = (int64_t)x + offset, dy = (int64_t)y + (int64_t)line * line_height;
        if (dx >= INT_MIN && dx <= INT_MAX && dy >= INT_MIN && dy <= INT_MAX)
            draw_glyph(rgb565, width, height, stride, (int)dx, (int)dy, cp, color);
        offset += advance;
    }
    return (line + 1) * line_height;
}
