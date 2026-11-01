#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Flash-resident 16px monochrome font; ASCII advances 8px, other glyphs 16px.
 * Bitmap data is an OFL-1.1 derivative of Noto Sans SC (see tools/fonts).
 * All drawing is transparent, clipped, RGB565 little endian; stride is bytes.
 * Calls must be made by the display owner, not concurrently on one buffer. */
enum { CJK_FONT_16_HEIGHT = 16 };

const uint16_t *cjk_font_16_glyph(uint32_t codepoint);
int cjk_font_16_advance(uint32_t codepoint);
size_t cjk_font_16_count(const char *utf8);
int cjk_font_16_measure(const char *utf8);

void cjk_font_16_draw(uint8_t *rgb565, int width, int height, int stride,
                      int x, int y, const char *utf8, uint16_t color);
/* Nearest-neighbor 24px rendering of the same flash-resident glyphs. */
void cjk_font_16_draw_24(uint8_t *rgb565, int width, int height, int stride,
                       int x, int y, const char *utf8, uint16_t color);
/* Wraps on character boundaries; returns height occupied, up to max_lines.
 * Long content is clipped to max_lines. Explicit newlines are respected. */
int cjk_font_16_draw_wrapped(uint8_t *rgb565, int width, int height, int stride,
                            int x, int y, const char *utf8, uint16_t color,
                            int max_width, int line_height, int max_lines);

#ifdef __cplusplus
}
#endif
