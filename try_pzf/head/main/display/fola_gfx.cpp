#include "fola_gfx.h"

#include "cjk_font_16.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

namespace fola {

void *cache_alloc(size_t bytes)
{
#ifdef ESP_PLATFORM
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return std::malloc(bytes);
#endif
}

Rect unite(const Rect &a, const Rect &b)
{
    if (a.empty()) return b;
    if (b.empty()) return a;
    return {std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}

namespace {

constexpr uint8_t kBayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

inline int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
inline float smoothstep(float a, float b, float v)
{
    const float t = clamp01((v - a) / (b - a));
    return t * t * (3 - 2 * t);
}
/* Coverage of a Gaussian-blurred edge at signed distance d (logistic fit). */
inline float soft_edge(float d, float sigma) { return 1.0f / (1.0f + std::exp(1.702f * d / sigma)); }

inline Rgb unpack(uint16_t p)
{
    const int r = p >> 11, g = (p >> 5) & 63, b = p & 31;
    return {uint8_t(r << 3 | r >> 2), uint8_t(g << 2 | g >> 4), uint8_t(b << 3 | b >> 2)};
}
inline Rgb mix(Rgb d, Rgb s, int alpha)
{
    return {uint8_t(d.r + (((s.r - d.r) * alpha) >> 8)), uint8_t(d.g + (((s.g - d.g) * alpha) >> 8)),
            uint8_t(d.b + (((s.b - d.b) * alpha) >> 8))};
}
inline Rgb screen_mix(Rgb d, Rgb s, int alpha)
{
    const int r = (s.r * alpha) >> 8, g = (s.g * alpha) >> 8, b = (s.b * alpha) >> 8;
    return {uint8_t(d.r + r - d.r * r / 255), uint8_t(d.g + g - d.g * g / 255), uint8_t(d.b + b - d.b * b / 255)};
}

// Figma Display-Background: #12202a -> #071016 (38%) -> #010407, running
// from top-left to bottom-right of the squircle box.
struct BackgroundTable {
    Rgb gradient[256];
    BackgroundTable()
    {
        const Rgb a = hex(0x12202a), b = hex(0x071016), c = hex(0x010407);
        for (int i = 0; i < 256; ++i) {
            const float t = i / 255.0f;
            const Rgb from = t < 0.38f ? a : b, to = t < 0.38f ? b : c;
            const float f = t < 0.38f ? t / 0.38f : (t - 0.38f) / 0.62f;
            gradient[i] = {uint8_t(from.r + (to.r - from.r) * f + 0.5f), uint8_t(from.g + (to.g - from.g) * f + 0.5f),
                           uint8_t(from.b + (to.b - from.b) * f + 0.5f)};
        }
    }
};
const BackgroundTable kBackground;

// Glass sheen over the top half: #c9f0f5 8% -> #93dae6 2% (28%) -> clear (55%).
struct Sheen {
    int r, g, b, keep; // premultiplied colour (x256) and remaining background weight (x256)
};
Sheen sheen_at(float ny)
{
    float alpha;
    Rgb color;
    if (ny < 0.28f) {
        const float f = clamp01(ny / 0.28f);
        alpha = 0.08f + (0.02f - 0.08f) * f;
        color = {uint8_t(0xc9 + (0x93 - 0xc9) * f), uint8_t(0xf0 + (0xda - 0xf0) * f), uint8_t(0xf5 + (0xe6 - 0xf5) * f)};
    } else if (ny < 0.55f) {
        alpha = 0.02f * (1 - (ny - 0.28f) / 0.27f);
        color = hex(0x93dae6);
    } else {
        return {0, 0, 0, 256};
    }
    const int a = int(alpha * 256 + 0.5f);
    return {color.r * a, color.g * a, color.b * a, 256 - a};
}

constexpr float kGradX = 0.474f, kGradY = 0.659f;

} // namespace

uint16_t pack_dithered(int x, int y, int r, int g, int b)
{
    const int d = kBayer[((y & 3) << 2) | (x & 3)];
    r = clamp255(r + (d >> 1) - 4);
    g = clamp255(g + (d >> 2) - 2);
    b = clamp255(b + (d >> 1) - 4);
    return uint16_t(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

Canvas::Canvas(uint8_t *bytes, int width, int height, int stride)
    : bytes_(bytes), width_(width), height_(height), stride_(stride),
      unit_(std::min(width, height) / 480.0f) {}

Rect Canvas::to_pixels(float x, float y, float w, float h) const
{
    Rect r{int(std::floor(x * unit_)), int(std::floor(y * unit_)), int(std::ceil((x + w) * unit_)),
           int(std::ceil((y + h) * unit_))};
    r.x0 = std::max(r.x0, 0);
    r.y0 = std::max(r.y0, 0);
    r.x1 = std::min(r.x1, width_);
    r.y1 = std::min(r.y1, height_);
    return r;
}

void Canvas::background(Rect a) const
{
    a.x0 = std::max(a.x0, 0);
    a.y0 = std::max(a.y0, 0);
    a.x1 = std::min(a.x1, width_);
    a.y1 = std::min(a.y1, height_);
    if (a.empty()) return;
    const float box_x = fig_x(62) * unit_, box_w = fig_len(406) * unit_, box_h = fig_len(412) * unit_;
    // Gradient index in 16.16 fixed point, stepped along the row.
    const int32_t step = int32_t(kGradX / box_w * 255 * 65536);
    for (int y = a.y0; y < a.y1; ++y) {
        const float ny = (y + 0.5f) / box_h;
        const Sheen s = sheen_at(ny);
        int32_t t = int32_t((kGradX * (a.x0 + 0.5f - box_x) / box_w + kGradY * ny) * 255 * 65536);
        uint16_t *out = row(y);
        for (int x = a.x0; x < a.x1; ++x, t += step) {
            const Rgb g = kBackground.gradient[t <= 0 ? 0 : (t >= (255 << 16) ? 255 : t >> 16)];
            out[x] = pack_dithered(x, y, (g.r * s.keep + s.r) >> 8, (g.g * s.keep + s.g) >> 8, (g.b * s.keep + s.b) >> 8);
        }
    }
}

Rgb Canvas::background_at(int x, int y) const
{
    const float box_x = fig_x(62) * unit_, box_w = fig_len(406) * unit_, box_h = fig_len(412) * unit_;
    const float ny = (y + 0.5f) / box_h;
    const float t = kGradX * (x + 0.5f - box_x) / box_w + kGradY * ny;
    const Rgb g = kBackground.gradient[int(clamp01(t) * 255)];
    const Sheen s = sheen_at(ny);
    return {uint8_t((g.r * s.keep + s.r) >> 8), uint8_t((g.g * s.keep + s.g) >> 8), uint8_t((g.b * s.keep + s.b) >> 8)};
}

Rgb Canvas::read(int x, int y) const { return unpack(row(y)[x]); }
void Canvas::write(int x, int y, Rgb c) const { row(y)[x] = pack_dithered(x, y, c.r, c.g, c.b); }
void Canvas::blend(int x, int y, Rgb c, int alpha) const
{
    if (alpha <= 0) return;
    write(x, y, alpha >= 256 ? c : mix(read(x, y), c, alpha));
}
void Canvas::screen(int x, int y, Rgb c, int alpha) const
{
    if (alpha > 0) write(x, y, screen_mix(read(x, y), c, std::min(alpha, 256)));
}

// ---- Shapes ----------------------------------------------------------------------

namespace {

/* Signed distance to a rounded rectangle, in design units. */
inline float round_rect_distance(float px, float py, float cx, float cy, float hw, float hh, float r)
{
    const float qx = std::fabs(px - cx) - (hw - r), qy = std::fabs(py - cy) - (hh - r);
    const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
}

template <typename Shade>
void for_each_pixel(const Canvas &c, float x, float y, float w, float h, Shade shade)
{
    const Rect r = c.to_pixels(x, y, w, h);
    const float inv = 1.0f / c.unit();
    for (int py = r.y0; py < r.y1; ++py)
        for (int px = r.x0; px < r.x1; ++px) shade(px, py, (px + 0.5f) * inv, (py + 0.5f) * inv);
}

} // namespace

void round_rect(const Canvas &c, float x, float y, float w, float h, float radius, Rgb color, float alpha)
{
    const float u = c.unit();
    for_each_pixel(c, x, y, w, h, [&](int px, int py, float dx, float dy) {
        const float d = round_rect_distance(dx, dy, x + w / 2, y + h / 2, w / 2, h / 2, radius) * u;
        c.blend(px, py, color, int(alpha * clamp01(0.5f - d) * 256));
    });
}

void round_rect_gradient(const Canvas &c, float x, float y, float w, float h, float radius,
                         Rgb top, float top_alpha, Rgb bottom, float bottom_alpha)
{
    const float u = c.unit();
    for_each_pixel(c, x, y, w, h, [&](int px, int py, float dx, float dy) {
        const float d = round_rect_distance(dx, dy, x + w / 2, y + h / 2, w / 2, h / 2, radius) * u;
        const float f = clamp01((dy - y) / h);
        const Rgb color = {uint8_t(top.r + (bottom.r - top.r) * f), uint8_t(top.g + (bottom.g - top.g) * f),
                           uint8_t(top.b + (bottom.b - top.b) * f)};
        const float a = top_alpha + (bottom_alpha - top_alpha) * f;
        c.blend(px, py, color, int(a * clamp01(0.5f - d) * 256));
    });
}

void round_rect_outline(const Canvas &c, float x, float y, float w, float h, float radius, Rgb color, float alpha)
{
    const float u = c.unit();
    for_each_pixel(c, x, y, w, h, [&](int px, int py, float dx, float dy) {
        const float d = round_rect_distance(dx, dy, x + w / 2, y + h / 2, w / 2, h / 2, radius) * u;
        c.blend(px, py, color, int(alpha * clamp01(1 - std::fabs(d + 0.5f)) * 256));
    });
}

void round_rect_glow(const Canvas &c, float x, float y, float w, float h, float radius,
                     float sigma, Rgb color, float alpha)
{
    const float m = sigma * 3;
    for_each_pixel(c, x - m, y - m, w + 2 * m, h + 2 * m, [&](int px, int py, float dx, float dy) {
        const float d = round_rect_distance(dx, dy, x + w / 2, y + h / 2, w / 2, h / 2, radius);
        c.screen(px, py, color, int(alpha * soft_edge(d, sigma) * 256));
    });
}

void circle(const Canvas &c, float cx, float cy, float r, Rgb color, float alpha)
{
    const float u = c.unit();
    for_each_pixel(c, cx - r - 1, cy - r - 1, 2 * r + 2, 2 * r + 2, [&](int px, int py, float dx, float dy) {
        const float d = (std::hypot(dx - cx, dy - cy) - r) * u;
        c.blend(px, py, color, int(alpha * clamp01(0.5f - d) * 256));
    });
}

void circle_glow(const Canvas &c, float cx, float cy, float r, float sigma, Rgb color, float alpha)
{
    const float m = r + sigma * 3;
    for_each_pixel(c, cx - m, cy - m, 2 * m, 2 * m, [&](int px, int py, float dx, float dy) {
        c.screen(px, py, color, int(alpha * soft_edge(std::hypot(dx - cx, dy - cy) - r, sigma) * 256));
    });
}

void sparkle(const Canvas &c, float cx, float cy, float size, Rgb color, float alpha)
{
    const float u = c.unit();
    circle_glow(c, cx, cy, size * 0.25f, size * 0.35f, color, alpha * 0.35f);
    for_each_pixel(c, cx - size, cy - size, 2 * size, 2 * size, [&](int px, int py, float dx, float dy) {
        // Concave four-point star: sqrt|x| + sqrt|y| <= sqrt(size).
        const float ax = std::fabs(dx - cx) / size, ay = std::fabs(dy - cy) / size;
        const float f = std::sqrt(ax) + std::sqrt(ay) - 1;
        c.blend(px, py, color, int(alpha * clamp01(0.5f - f * size * 0.35f * u) * 256));
    });
}

// ---- Text ------------------------------------------------------------------------

namespace {

uint32_t next_codepoint(const char *&p)
{
    const unsigned char *s = reinterpret_cast<const unsigned char *>(p);
    if (!*s) return 0;
    unsigned count;
    uint32_t cp;
    if (*s < 0x80) { ++p; return *s; }
    if (*s >= 0xC2 && *s <= 0xDF) { count = 2; cp = *s & 0x1F; }
    else if (*s >= 0xE0 && *s <= 0xEF) { count = 3; cp = *s & 0x0F; }
    else if (*s >= 0xF0 && *s <= 0xF4) { count = 4; cp = *s & 0x07; }
    else { ++p; return 0xFFFD; }
    for (unsigned i = 1; i < count; ++i) {
        if ((s[i] & 0xC0) != 0x80) { ++p; return 0xFFFD; }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    p += count;
    return cp;
}

/* Same placeholder box as cjk_font_16 for glyphs outside the subset. */
inline bool glyph_bit(const uint16_t *rows, int col, int row, int advance)
{
    if (col < 0 || row < 0 || col >= advance || row >= 16) return false;
    const uint16_t bits = rows ? rows[row] : (row == 1 || row == 14 ? 0x7FFE : (row > 1 && row < 14 ? 0x4002 : 0));
    return bits & (0x8000u >> col);
}

void draw_glyph(const Canvas &c, int left, int top, uint32_t cp, float scale, Rgb color, float alpha)
{
    const uint16_t *rows = cjk_font_16_glyph(cp);
    const int advance = cjk_font_16_advance(cp);
    const int w = int(std::ceil(advance * scale)), h = int(std::ceil(16 * scale));
    const float inv = 1.0f / scale;
    for (int oy = 0; oy < h; ++oy) {
        const int py = top + oy;
        if (py < 0 || py >= c.height()) continue;
        const float gy = (oy + 0.5f) * inv - 0.5f;
        const int y0 = int(std::floor(gy));
        const float fy = gy - y0;
        for (int ox = 0; ox < w; ++ox) {
            const int px = left + ox;
            if (px < 0 || px >= c.width()) continue;
            const float gx = (ox + 0.5f) * inv - 0.5f;
            const int x0 = int(std::floor(gx));
            const float fx = gx - x0;
            const float top_row = glyph_bit(rows, x0, y0, advance) * (1 - fx) + glyph_bit(rows, x0 + 1, y0, advance) * fx;
            const float bottom_row = glyph_bit(rows, x0, y0 + 1, advance) * (1 - fx) + glyph_bit(rows, x0 + 1, y0 + 1, advance) * fx;
            const float coverage = top_row * (1 - fy) + bottom_row * fy;
            if (coverage <= 0.2f) continue;
            // A low threshold thickens the regular-weight bitmap towards Figma's bold.
            c.blend(px, py, color, int(alpha * smoothstep(0.2f, 0.6f, coverage) * 256));
        }
    }
}

} // namespace

float text_width(const char *utf8, float scale)
{
    int width = 0;
    if (utf8)
        for (const char *p = utf8; *p;) width += cjk_font_16_advance(next_codepoint(p));
    return width * scale;
}

void text(const Canvas &c, float x, float y, const char *utf8, float scale, Rgb color, float alpha)
{
    if (!utf8) return;
    const float s = scale * c.unit();
    float pen = x * c.unit();
    const int top = int(std::lround(y * c.unit()));
    for (const char *p = utf8; *p;) {
        const uint32_t cp = next_codepoint(p);
        draw_glyph(c, int(std::lround(pen)), top, cp, s, color, alpha);
        pen += cjk_font_16_advance(cp) * s;
    }
}

void text_centered(const Canvas &c, float cx, float y, const char *utf8, float scale, Rgb color, float alpha)
{
    text(c, cx - text_width(utf8, scale) / 2, y, utf8, scale, color, alpha);
}

int wrap_two_lines(const char *utf8, float scale, float max_width, char lines[2][kLineBytes])
{
    lines[0][0] = lines[1][0] = 0;
    if (!utf8 || !*utf8) return 0;
    constexpr int kMax = kLineBytes; // a step never has more characters than bytes
    const char *starts[kMax + 1];
    float widths[kMax];
    int n = 0;
    float total = 0;
    for (const char *p = utf8; *p && n < kMax;) {
        starts[n] = p;
        widths[n] = cjk_font_16_advance(next_codepoint(p)) * scale;
        total += widths[n++];
        starts[n] = p;
    }
    auto copy = [&](char *out, int from, int to) {
        size_t bytes = std::min<size_t>(size_t(starts[to] - starts[from]), kLineBytes - 1);
        std::memcpy(out, starts[from], bytes);
        out[bytes] = 0;
    };
    if (total <= max_width) {
        copy(lines[0], 0, n);
        return 1;
    }
    // Balanced split: the break that makes the longer line shortest.
    int best = -1;
    float best_longest = 1e9f, first = 0;
    for (int k = 1; k < n; ++k) {
        first += widths[k - 1];
        const float longest = std::max(first, total - first);
        if (first <= max_width && total - first <= max_width && longest < best_longest) {
            best = k;
            best_longest = longest;
        }
    }
    if (best < 0) { // Cannot fit in two lines: fill the first, cut the second.
        float w = 0;
        best = 0;
        while (best < n && w + widths[best] <= max_width) w += widths[best++];
        int end = best;
        w = 0;
        while (end < n && w + widths[end] <= max_width) w += widths[end++];
        copy(lines[0], 0, best);
        copy(lines[1], best, end);
        return 2;
    }
    copy(lines[0], 0, best);
    copy(lines[1], best, n);
    return 2;
}

// ---- Track -----------------------------------------------------------------------

const TrackStyle kGreenTrack = {hex(0x263139), 0.86f, hex(0x73bf48), hex(0x82d351), hex(0xa0df7e),
                                hex(0x82d351), hex(0xd8f2c9)};

namespace {

// Centre line of the 12px groove: a superellipse (exponent 3) with Figma
// semi-axes 179 x 182 around the panel centre.
constexpr float kTrackA = fig_len(179), kTrackB = fig_len(182);
constexpr float kBand = 28;                    // repainted half-width around the centre line
constexpr float kGrooveHalf = fig_len(6);      // 12px groove
constexpr float kArcHalf = fig_len(8);         // 16px arc
constexpr int kSamples = 256;                  // arc-length table around the loop

struct LoopTable {
    float x[kSamples + 1], y[kSamples + 1], position[kSamples + 1]; // position 0..1
    LoopTable()
    {
        float length = 0;
        for (int i = 0; i <= kSamples; ++i) {
            const float a = 6.2831853f * i / kSamples;
            const float dx = std::sin(a), dy = -std::cos(a);
            const float f = std::pow(std::fabs(dx / kTrackA), 3.0f) + std::pow(std::fabs(dy / kTrackB), 3.0f);
            const float r = 1.0f / std::cbrt(f);
            x[i] = dx * r;
            y[i] = dy * r;
            if (i) length += std::hypot(x[i] - x[i - 1], y[i] - y[i - 1]);
            position[i] = length;
        }
        for (float &p : position) p /= length;
    }
    /* Loop position (0..1) of a direction angle measured clockwise from top. */
    float at_angle(float a) const
    {
        const float f = a / 6.2831853f * kSamples;
        const int i = std::min(kSamples - 1, std::max(0, int(f)));
        return position[i] + (position[i + 1] - position[i]) * (f - i);
    }
    void point(float p, float &px, float &py) const
    {
        int i = int(std::upper_bound(position, position + kSamples + 1, p) - position) - 1;
        i = std::min(kSamples - 1, std::max(0, i));
        const float span = position[i + 1] - position[i];
        const float f = span > 0 ? (p - position[i]) / span : 0;
        px = x[i] + (x[i + 1] - x[i]) * f;
        py = y[i] + (y[i + 1] - y[i]) * f;
    }
};
const LoopTable &loop()
{
    static const LoopTable table;
    return table;
}

/* Signed distance from the track centre line (ray approximation) in design units. */
float track_distance(float x, float y)
{
    const float f = std::pow(std::fabs(x / kTrackA), 3.0f) + std::pow(std::fabs(y / kTrackB), 3.0f);
    if (f < 1e-9f) return -kTrackA;
    return std::hypot(x, y) * (1 - 1 / std::cbrt(f));
}

/* Superellipse x extent of a row for a curve offset by `offset`; -1 if the row misses it. */
float row_extent(float y, float offset)
{
    const float a = kTrackA + offset, b = kTrackB + offset;
    const float v = std::fabs(y) / b;
    if (v >= 1) return -1;
    return a * std::cbrt(1 - v * v * v);
}

} // namespace

float Track::inner_clearance() { return kTrackA - kBand; }

bool Track::inner_span(const Canvas &c, int py, int &x0, int &x1)
{
    const float u = c.unit();
    const float extent = row_extent((py + 0.5f) / u - 240, -kBand - 2);
    if (extent <= 0) return false;
    x0 = std::max(0, int(std::ceil((240 - extent) * u)));
    x1 = std::min(c.width(), int(std::floor((240 + extent) * u)));
    return x1 > x0;
}

void Track::release()
{
    std::free(runs_);
    std::free(distance_);
    std::free(position_);
    runs_ = nullptr;
    distance_ = nullptr;
    position_ = nullptr;
    failed_ = false;
    run_count_ = built_width_ = built_height_ = 0;
}

bool Track::build(const Canvas &c)
{
    if (failed_) return false;
    if (runs_ && built_width_ == c.width() && built_height_ == c.height()) return true;
    release();
    const float u = c.unit(), inv = 1 / u;
    // Pass 1 counts pixels, pass 2 fills the tables.
    for (int pass = 0; pass < 2; ++pass) {
        uint32_t pixels = 0;
        int count = 0;
        for (int py = 0; py < c.height(); ++py) {
            const float y = (py + 0.5f) * inv - 240;
            const float outer = row_extent(y, kBand + 2), inner = row_extent(y, -kBand - 2);
            if (outer < 0) continue;
            int spans[2][2];
            int span_count = 0;
            auto add = [&](float from, float to) {
                spans[span_count][0] = std::max(0, int(std::floor((240 + from) * u)));
                spans[span_count][1] = std::min(c.width(), int(std::ceil((240 + to) * u)));
                if (spans[span_count][1] > spans[span_count][0]) ++span_count;
            };
            if (inner < 0) {
                add(-outer, outer);
            } else {
                add(-outer, -inner);
                add(inner, outer);
            }
            for (int s = 0; s < span_count; ++s) {
                const int length = spans[s][1] - spans[s][0];
                if (pass) {
                    runs_[count] = {int16_t(py), int16_t(spans[s][0]), int16_t(length), pixels};
                    for (int i = 0; i < length; ++i) {
                        const float x = (spans[s][0] + i + 0.5f) * inv - 240;
                        const float d = track_distance(x, y);
                        distance_[pixels + i] = std::fabs(d) > kBand ? 127 : int8_t(std::lround(d * 4));
                        float angle = std::atan2(x, -y);
                        if (angle < 0) angle += 6.2831853f;
                        position_[pixels + i] = uint16_t(std::min(65535.0f, loop().at_angle(angle) * 65535));
                    }
                }
                ++count;
                pixels += length;
            }
        }
        if (!pass) {
            runs_ = static_cast<Run *>(cache_alloc(sizeof(Run) * count));
            distance_ = static_cast<int8_t *>(cache_alloc(pixels));
            position_ = static_cast<uint16_t *>(cache_alloc(pixels * sizeof(uint16_t)));
            if (!runs_ || !distance_ || !position_) {
                release();
                failed_ = true;
                return false;
            }
            run_count_ = count;
        }
    }
    built_width_ = c.width();
    built_height_ = c.height();
    return true;
}

Rect Track::draw(const Canvas &c, float fraction, const TrackStyle &style)
{
    if (!build(c)) return {};
    fraction = clamp01(fraction);
    const float u = c.unit(), inv = 1 / u;
    // Profiles indexed by distance in quarter units.
    int glow[4 * int(kBand) + 1], highlight[4 * int(kBand) + 1];
    for (int i = 0; i <= 4 * int(kBand); ++i) {
        const float d = i / 4.0f;
        glow[i] = int(0.55f * std::exp(-d * d / (2 * 11.0f * 11.0f)) * 256);
        highlight[i] = int(0.36f * std::exp(-d * d / (2 * 2.5f * 2.5f)) * 256);
    }
    const uint32_t end = uint32_t(fraction * 65535);
    float end_x, end_y;
    loop().point(fraction, end_x, end_y);
    const float start_x = 0, start_y = -kTrackB;
    const float cap_reach = kBand + 1;
    Rect area;
    for (int r = 0; r < run_count_; ++r) {
        const Run &run = runs_[r];
        const int py = run.y;
        const float y = (py + 0.5f) * inv - 240;
        const bool near_start = std::fabs(y - start_y) < cap_reach;
        const bool near_end = fraction > 0 && std::fabs(y - end_y) < cap_reach;
        uint16_t *out = c.row(py);
        for (int i = 0; i < run.length; ++i) {
            const int px = run.x0 + i;
            const int8_t sd = distance_[run.offset + i];
            const Rgb bg = c.background_at(px, py);
            if (sd == 127) {
                out[px] = pack_dithered(px, py, bg.r, bg.g, bg.b);
                continue;
            }
            const float d = std::fabs(sd / 4.0f);
            const uint16_t pos = position_[run.offset + i];
            // Distance to the lit arc: along it, the centre-line distance;
            // beyond its ends, the distance to the round caps.
            float arc = 1e9f;
            if (fraction > 0) {
                if (pos <= end) arc = d;
                const float x = (px + 0.5f) * inv - 240;
                if (near_start) arc = std::min(arc, std::hypot(x - start_x, y - start_y));
                if (near_end) arc = std::min(arc, std::hypot(x - end_x, y - end_y));
            }
            Rgb color = mix(bg, style.groove, int(style.groove_alpha * clamp01((kGrooveHalf - d) * u + 0.5f) * 256));
            if (arc < kBand) {
                const int q = int(arc * 4);
                color = screen_mix(color, style.glow, glow[q]);
                const float t = pos <= end && end ? float(pos) / end : (pos > end && pos > 32768 ? 0.0f : 1.0f);
                const Rgb from = t < 0.55f ? style.start : style.mid, to = t < 0.55f ? style.mid : style.end;
                const float f = t < 0.55f ? t / 0.55f : (t - 0.55f) / 0.45f;
                const Rgb arc_color = {uint8_t(from.r + (to.r - from.r) * f), uint8_t(from.g + (to.g - from.g) * f),
                                       uint8_t(from.b + (to.b - from.b) * f)};
                color = mix(color, arc_color, int(clamp01((kArcHalf - arc) * u + 0.5f) * 256));
                color = screen_mix(color, style.highlight, highlight[q]);
            }
            out[px] = pack_dithered(px, py, color.r, color.g, color.b);
        }
        area = unite(area, {run.x0, py, run.x0 + run.length, py + 1});
    }
    return area;
}

} // namespace fola
