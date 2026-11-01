#include "cjk_font_16.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned ink(const uint8_t *buffer, size_t size)
{
    unsigned count = 0;
    for (size_t i = 0; i < size; ++i) if (buffer[i]) ++count;
    return count;
}

int main(void)
{
    assert(cjk_font_16_count("擦净桌面并摆回常用品") == 10);
    assert(cjk_font_16_count("第1/3步") == 5);
    assert(cjk_font_16_count("\xF0\x9F\x98\x80") == 1);
    assert(cjk_font_16_measure("第1/3步") == 56);
    assert(cjk_font_16_measure("ABC\n擦净") == 32);
    assert(cjk_font_16_glyph(0x684C)); /* 桌 */
    assert(cjk_font_16_glyph(0x5206)); /* 分 */
    assert(!cjk_font_16_glyph(0x1F600));
    assert(memcmp(cjk_font_16_glyph(0x684C), cjk_font_16_glyph(0x5206), 32));

    enum { W = 35, H = 35, STRIDE = 80, GUARD = 23, LEN = STRIDE * H };
    uint8_t storage[GUARD + LEN + GUARD];
    memset(storage, 0xA5, sizeof storage);
    uint8_t *buffer = storage + GUARD;
    for (int row = 0; row < H; ++row) memset(buffer + row * STRIDE, 0, W * 2);
    cjk_font_16_draw(buffer, W, H, STRIDE, -5, -4, "桌面", 0xF800);
    cjk_font_16_draw(buffer, W, H, STRIDE, 30, 30, "整理", 0x07E0);
    assert(ink(buffer, LEN) > (STRIDE - W * 2) * H);
    for (int row = 0; row < H; ++row)
        for (int i = W * 2; i < STRIDE; ++i) assert(buffer[row * STRIDE + i] == 0xA5);
    for (int i = 0; i < GUARD; ++i) assert(storage[i] == 0xA5 && storage[GUARD + LEN + i] == 0xA5);

    uint8_t missing[16 * 16 * 2] = {0};
    cjk_font_16_draw(missing, 16, 16, 32, 0, 0, "\xF0\x9F\x98\x80", 0xFFFF);
    assert(ink(missing, sizeof missing) > 0);
    memset(missing, 0, sizeof missing);
    cjk_font_16_draw(missing, 16, 16, 32, 0, 0, " ", 0xFFFF);
    assert(ink(missing, sizeof missing) == 0);
    /* Truncated/overlong UTF-8 must not read beyond its terminator or loop. */
    assert(cjk_font_16_count("\xE4\xB8") == 2);
    assert(cjk_font_16_count("\xC0\x80") == 2);
    assert(cjk_font_16_draw_wrapped(buffer, W, H, STRIDE, 0, 0, "桌面整理", 0xFFFF, 16, 18, 2) == 36);
    puts("CJK16: Unicode lookup, metrics, clipped RGB565 drawing, missing glyphs and wrapping passed");
    return 0;
}
