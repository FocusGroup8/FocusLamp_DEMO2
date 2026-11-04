#pragma once

// FOLA screen graphics shared by the task guide and the "fola" eye role.
// Visual source: Figma 台灯 TfPVhZKtjCjZ3CZahHzawc, page 眼睛 5:49
// (screen states 5:732, R04 focus island 5:3735, green progress 5:5293,
// TASK START/DONE 5:6911/5:6879).
//
// Everything draws straight into the existing LVGL RGB565 canvas. No extra
// full-screen buffer: callers repaint only the rectangles that changed.
// Layout coordinates are in the 480x480 design space; Canvas::unit() maps
// them to the real panel.

#include <cstddef>
#include <cstdint>

namespace fola {

struct Rgb {
    uint8_t r, g, b;
};
constexpr Rgb hex(uint32_t c) { return {uint8_t(c >> 16), uint8_t(c >> 8), uint8_t(c)}; }

/* Half-open pixel rectangle on the panel. */
struct Rect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool empty() const { return x1 <= x0 || y1 <= y0; }
};
Rect unite(const Rect &a, const Rect &b);

// Figma frames are 540x499 with the display squircle at (62,48) 406x412.
// The squircle is fitted to the panel by height; calibrate on hardware.
constexpr float kFigmaScale = 480.0f / 412.0f;
constexpr float fig_x(float x) { return (x - 62) * kFigmaScale + (480 - 406 * kFigmaScale) / 2; }
constexpr float fig_y(float y) { return (y - 48) * kFigmaScale; }
constexpr float fig_len(float v) { return v * kFigmaScale; }

/* Platform allocation for caches (PSRAM on ESP32-P4). May return null. */
void *cache_alloc(size_t bytes);

class Canvas {
public:
    Canvas(uint8_t *bytes, int width, int height, int stride);

    int width() const { return width_; }
    int height() const { return height_; }
    float unit() const { return unit_; }
    Rect bounds() const { return {0, 0, width_, height_}; }
    /* Design-space box to the panel rectangle that covers it. */
    Rect to_pixels(float x, float y, float w, float h) const;
    uint16_t *row(int y) const { return reinterpret_cast<uint16_t *>(bytes_ + size_t(y) * stride_); }

    /* Display-Background gradient + glass sheen, ordered-dithered to RGB565. */
    void background(Rect area) const;
    /* Background colour before dithering, for compositing in 8 bit. */
    Rgb background_at(int x, int y) const;

    Rgb read(int x, int y) const;
    void write(int x, int y, Rgb c) const;
    /* alpha 0..256 */
    void blend(int x, int y, Rgb c, int alpha) const;
    /* Figma SCREEN blend, alpha 0..256 */
    void screen(int x, int y, Rgb c, int alpha) const;

private:
    uint8_t *bytes_;
    int width_, height_, stride_;
    float unit_;
};

uint16_t pack_dithered(int x, int y, int r, int g, int b);

// ---- Shapes (design-space units) -------------------------------------------

void round_rect(const Canvas &c, float x, float y, float w, float h, float radius, Rgb color, float alpha);
void round_rect_gradient(const Canvas &c, float x, float y, float w, float h, float radius,
                         Rgb top, float top_alpha, Rgb bottom, float bottom_alpha);
void round_rect_outline(const Canvas &c, float x, float y, float w, float h, float radius, Rgb color, float alpha);
/* Soft light around a rounded rect, Figma layer blur approximated with sigma. */
void round_rect_glow(const Canvas &c, float x, float y, float w, float h, float radius,
                     float sigma, Rgb color, float alpha);
void circle(const Canvas &c, float cx, float cy, float r, Rgb color, float alpha);
void circle_glow(const Canvas &c, float cx, float cy, float r, float sigma, Rgb color, float alpha);
/* Four-point sparkle from the C09 star halo celebration. */
void sparkle(const Canvas &c, float cx, float cy, float size, Rgb color, float alpha);

// ---- Text --------------------------------------------------------------------

// The 16px Noto Sans SC bitmap is upscaled with bilinear coverage and a soft
// threshold, which gives anti-aliased 24/32/40px text without a second font.
constexpr int kLineBytes = 80;
float text_width(const char *utf8, float scale);
void text(const Canvas &c, float x, float y, const char *utf8, float scale, Rgb color, float alpha = 1);
void text_centered(const Canvas &c, float cx, float y, const char *utf8, float scale, Rgb color, float alpha = 1);
/* Splits into at most two balanced lines no wider than max_width.
 * Returns the line count; anything that still does not fit is cut. */
int wrap_two_lines(const char *utf8, float scale, float max_width, char lines[2][kLineBytes]);

// ---- Progress track ------------------------------------------------------------

struct TrackStyle {
    Rgb groove;
    float groove_alpha;
    Rgb start, mid, end;   // gradient along the lit arc
    Rgb glow, highlight;
};
extern const TrackStyle kGreenTrack;

/* The squircle progress track 18px inside the display edge (Figma 5:5307,
 * 5:5314): a 12px groove, and a 16px round-capped arc that starts at top
 * centre and runs clockwise for `fraction` of the loop.
 * Geometry is computed once into a PSRAM band table, so a redraw is a cheap
 * table walk. Returns the repainted rectangle. */
class Track {
public:
    Rect draw(const Canvas &c, float fraction, const TrackStyle &style);
    /* Builds the band table without drawing (tens of ms, once). */
    bool prepare(const Canvas &c) { return build(c); }
    void release();
    /* Design-space distance from the panel centre to the outer glow on the
     * vertical/horizontal axis. Content inside this box never touches it. */
    static float inner_clearance();
    /* Pixels of row py that lie inside the band's inner edge; false if none.
     * Renderers inside the track clip to this so they never erase it. */
    static bool inner_span(const Canvas &c, int py, int &x0, int &x1);

private:
    bool build(const Canvas &c);
    struct Run {
        int16_t y, x0, length;
        uint32_t offset;
    };
    Run *runs_ = nullptr;
    int8_t *distance_ = nullptr;   // signed distance to the centre line, 1/4 px
    uint16_t *position_ = nullptr; // 0..65535 around the loop from top centre
    int run_count_ = 0;
    int built_width_ = 0, built_height_ = 0;
    bool failed_ = false;
};

} // namespace fola
