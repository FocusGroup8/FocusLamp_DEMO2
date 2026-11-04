#include "fola_eyes.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace fola {

// Colours copied from the Figma layers (Ambient-Spill, Bloom-Wide,
// Bloom-Tight, OuterBody radial, CoreTransition, InnerCore radial,
// CenterHighlight). Highlights of the spectrum variants are derived.
const Palette kTeal = {hex(0x57d4e2), hex(0x6fdae6), hex(0x88e1eb),
                       {hex(0xa3d9df), hex(0x84cad2), hex(0x64a5ac)}, hex(0x99d1d7),
                       {hex(0xc7e7ea), hex(0xabd8dd), hex(0x93c9cf), hex(0x7dbbc2)}, hex(0xf0f9fa)};
const Palette kGreen = {hex(0x82d351), hex(0x8fd866), hex(0xa0df7e),
                        {hex(0xadd699), hex(0x95c77a), hex(0x73a25b)}, hex(0xa4cd90),
                        {hex(0xcae2be), hex(0xb2d4a2), hex(0x9dc689), hex(0x8bb774)}, hex(0xecf3e8)};
const Palette kLavender = {hex(0xd3afef), hex(0xd9b8f2), hex(0xe0c3f6),
                           {hex(0xd8c6e6), hex(0xc9b2db), hex(0xa48fb4)}, hex(0xd0bddf),
                           {hex(0xe6dbef), hex(0xd6c7e3), hex(0xc8b6d6), hex(0xb9a5ca)}, hex(0xf6f0fa)};
const Palette kLemon = {hex(0xd0c52c), hex(0xd5cc4f), hex(0xdcd56f),
                        {hex(0xd5d194), hex(0xc6c070), hex(0xa19c51)}, hex(0xcdc98a),
                        {hex(0xe4e2be), hex(0xd4d19e), hex(0xc5c184), hex(0xb7b26c)}, hex(0xf7f6e6)};
const Palette kOrange = {hex(0xff9d5d), hex(0xffa870), hex(0xffb787),
                         {hex(0xfabfa1), hex(0xf3a882), hex(0xc98663)}, hex(0xf3b697),
                         {hex(0xfbd7c5), hex(0xf4c2a9), hex(0xeaaf91), hex(0xdf9d7b)}, hex(0xfff1ea)};

EyeLayout neutral_layout(const Palette &palette)
{
    // Body boxes at x135/x286, y164 in the 540x499 frame: centre (265, 238).
    return {&palette, 1.0f, fig_x(265), fig_y(238)};
}

namespace {

// Figma geometry, in Figma px around the eye centre.
constexpr float kSpacing = 151;                  // left/right eye centres
constexpr float kBodyHalfW = 52.5f, kBodyHalfH = 74;
constexpr float kSpriteHalfW = 94, kSpriteHalfH = 118; // spill + blur tail

inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
inline float smooth(float t)
{
    t = clamp01(t);
    return t * t * (3 - 2 * t);
}
inline float soft_edge(float d, float sigma) { return 1.0f / (1.0f + std::exp(1.702f * d / sigma)); }

/* Ray distance to a superellipse (exponent 4) boundary, Figma px. */
float distance4(float x, float y, float hw, float hh)
{
    const float u = x / hw, v = y / hh;
    const float f = u * u * u * u + v * v * v * v;
    if (f < 1e-12f) return -std::min(hw, hh);
    return std::hypot(x, y) * (1 - 1 / std::sqrt(std::sqrt(f)));
}

struct Color3 {
    float r, g, b;
};
Color3 to3(Rgb c) { return {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f}; }
Color3 lerp3(Color3 a, Color3 b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
Color3 stops(const Rgb *colors, const float *at, int count, float t)
{
    if (t <= at[0]) return to3(colors[0]);
    for (int i = 1; i < count; ++i)
        if (t <= at[i]) return lerp3(to3(colors[i - 1]), to3(colors[i]), (t - at[i - 1]) / (at[i] - at[i - 1]));
    return to3(colors[count - 1]);
}
void normal(Color3 &c, Color3 s, float a) { c = lerp3(c, s, clamp01(a)); }
void screen(Color3 &c, Color3 s, float a)
{
    a = clamp01(a);
    c = {1 - (1 - c.r) * (1 - s.r * a), 1 - (1 - c.g) * (1 - s.g * a), 1 - (1 - c.b) * (1 - s.b * a)};
}

struct Sprite {
    const Palette *palette = nullptr;
    float scale = 0, unit = 0;
    bool happy = false;
    int w = 0, h = 0;
    uint8_t *rgb = nullptr;  // composited on black, 3 bytes per pixel
    uint32_t used = 0;
};
constexpr int kSlots = 5;
Sprite sprites[kSlots];
uint32_t use_clock = 0;

// Happy arch: an open-bottomed U (Figma px, eye-centred) taken out of every
// layer, grown by each layer's inset so the core keeps its margin. Its round
// top passes through (0, 10) and it meets the bottom edge at x = +-37,
// leaving two rounded legs like the Figma completion eyes. Open at the
// bottom so the glow cannot close it into a hole.
constexpr float kArchY = 52.7f, kArchR = 42.7f;

/* Layer distance with the arch removed when happy. */
inline float shaped(float d, float x, float y, float inset, bool happy)
{
    if (!happy) return d;
    const float r = kArchR + inset;
    const float notch = y > kArchY ? std::fabs(x) - r : std::hypot(x, y - kArchY) - r;
    return std::max(d, -notch);
}

void render(Sprite &s)
{
    const Palette &p = *s.palette;
    const float k = kFigmaScale * s.scale * s.unit; // panel px per Figma px
    const float body_at[3] = {0, 0.54f, 1}, core_at[4] = {0, 0.48f, 0.82f, 1};
    uint8_t *out = s.rgb;
    for (int j = 0; j < s.h; ++j) {
        const float y = (j + 0.5f - s.h / 2.0f) / k;
        for (int i = 0; i < s.w; ++i, out += 3) {
            const float x = (i + 0.5f - s.w / 2.0f) / k;
            Color3 c = {0, 0, 0};
            const bool h = s.happy;
            screen(c, to3(p.spill), 0.38f * soft_edge(shaped(distance4(x, y, 63.5f, 88), x, y, -11, h), 10));
            const float body = shaped(distance4(x, y, kBodyHalfW, kBodyHalfH), x, y, 0, h);
            screen(c, to3(p.bloom_wide), 0.72f * soft_edge(body, 12));
            screen(c, to3(p.bloom_tight), 0.52f * soft_edge(body, 5));
            if (body * k < 1) {
                const float t = std::hypot((x + 2.1f) / 81.9f, (y + 11.8f) / 115.4f);
                normal(c, stops(p.body, body_at, 3, t), 0.5f - body * k);
            }
            const float core = shaped(distance4(x, y, 41.5f, 60), x, y, 11, h);
            normal(c, to3(p.core_glow), 0.46f * soft_edge(core, 5.5f));
            const float t = std::hypot((x + 1.6f) / 68.0f, (y + 8.5f) / 98.4f);
            normal(c, stops(p.core, core_at, 4, t), 0.96f * soft_edge(core, 2.8f));
            normal(c, to3(p.highlight), 0.32f * soft_edge(shaped(distance4(x, y, 30.5f, 46), x, y, 22, h), 5.5f));
            out[0] = uint8_t(c.r * 255 + 0.5f);
            out[1] = uint8_t(c.g * 255 + 0.5f);
            out[2] = uint8_t(c.b * 255 + 0.5f);
        }
    }
}

const Sprite *sprite_for(const Canvas &c, const EyeLayout &layout, bool happy)
{
    Sprite *free_slot = nullptr, *oldest = &sprites[0];
    for (Sprite &s : sprites) {
        if (s.palette == layout.palette && s.scale == layout.scale && s.unit == c.unit() && s.happy == happy) {
            s.used = ++use_clock;
            return &s;
        }
        if (!s.rgb && !free_slot) free_slot = &s;
        if (s.used < oldest->used) oldest = &s;
    }
    const float k = kFigmaScale * layout.scale * c.unit();
    const int w = 2 * int(std::ceil(kSpriteHalfW * k)), h = 2 * int(std::ceil(kSpriteHalfH * k));
    Sprite *s = free_slot ? free_slot : oldest;
    if (s->rgb && s->w * s->h < w * h) {
        std::free(s->rgb);
        s->rgb = nullptr;
    }
    if (!s->rgb) s->rgb = static_cast<uint8_t *>(cache_alloc(size_t(w) * h * 3));
    if (!s->rgb) {
        s->palette = nullptr;
        return nullptr;
    }
    s->palette = layout.palette;
    s->scale = layout.scale;
    s->unit = c.unit();
    s->happy = happy;
    s->w = w;
    s->h = h;
    s->used = ++use_clock;
    render(*s);
    return s;
}

/* One composited row of 8-bit pixels for the area being repainted. */
uint8_t *row_buffer(int width)
{
    static uint8_t *buffer = nullptr;
    static int capacity = 0;
    if (width > capacity) {
        std::free(buffer);
        buffer = static_cast<uint8_t *>(cache_alloc(size_t(width) * 3));
        capacity = buffer ? width : 0;
    }
    return buffer;
}

struct EyePlacement {
    float cx, cy; // panel px
    const EyeShape *shape;
    const Sprite *sprite;
    bool left;
};

} // namespace

Rect eyes_area(const Canvas &c, const EyeLayout &layout)
{
    const float half_w = fig_len(kSpacing / 2 + kSpriteHalfW) * layout.scale + kMaxGaze;
    const float half_h = fig_len(kSpriteHalfH) * layout.scale + kMaxGazeY + kMaxLift;
    return c.to_pixels(layout.cx - half_w, layout.cy - half_h, 2 * half_w, 2 * half_h);
}

void prepare_eyes(const Canvas &c, const EyeLayout &layout, bool happy) { sprite_for(c, layout, happy); }

bool draw_eyes(const Canvas &c, const EyeLayout &layout, const EyePose &pose)
{
    const Rect area = eyes_area(c, layout);
    if (area.empty()) return true;
    const Sprite *left_sprite = sprite_for(c, layout, pose.left.happy);
    const Sprite *right_sprite = sprite_for(c, layout, pose.right.happy);
    uint8_t *line = row_buffer(area.x1 - area.x0);
    if (!left_sprite || !right_sprite || !line) {
        c.background(area);
        return false;
    }
    const float u = c.unit();
    const float gx = std::max(-kMaxGaze, std::min(kMaxGaze, pose.gaze_x));
    const float gy = std::max(-kMaxGazeY, std::min(kMaxGazeY, pose.gaze_y));
    const float lift = std::max(-kMaxLift, std::min(kMaxLift, pose.lift));
    const float half_spacing = fig_len(kSpacing / 2) * layout.scale;
    const EyePlacement eyes[2] = {
        {std::round((layout.cx - half_spacing + gx) * u), (layout.cy + gy + lift) * u, &pose.left, left_sprite, true},
        {std::round((layout.cx + half_spacing + gx) * u), (layout.cy + gy + lift) * u, &pose.right, right_sprite, false}};
    const float k = kFigmaScale * layout.scale * u;
    const float body_a = kBodyHalfW * k, body_b = kBodyHalfH * k;
    const int width = area.x1 - area.x0;

    for (int y = area.y0; y < area.y1; ++y) {
        int clip0 = area.x0, clip1 = area.x1;
        if (layout.inside_track) {
            int in0, in1;
            if (!Track::inner_span(c, y, in0, in1)) continue;
            clip0 = std::max(clip0, in0);
            clip1 = std::min(clip1, in1);
            if (clip1 <= clip0) continue;
        }
        // Background in 8 bit, then both eyes screened on top.
        const Rgb first = c.background_at(area.x0, y), last = c.background_at(area.x1 - 1, y);
        for (int i = 0; i < width; ++i) { // the gradient is linear along a row
            const int f = width > 1 ? i * 256 / (width - 1) : 0;
            line[i * 3 + 0] = uint8_t(first.r + (((last.r - first.r) * f) >> 8));
            line[i * 3 + 1] = uint8_t(first.g + (((last.g - first.g) * f) >> 8));
            line[i * 3 + 2] = uint8_t(first.b + (((last.b - first.b) * f) >> 8));
        }
        for (const EyePlacement &e : eyes) {
            const EyeShape &shape = *e.shape;
            const Sprite *s = e.sprite;
            const float open = std::max(0.04f, std::min(1.15f, shape.open));
            const float dy = y + 0.5f - e.cy;
            const float sy = dy / open + s->h / 2.0f - 0.5f;
            if (sy <= -1 || sy >= s->h) continue;
            const int y0 = int(std::floor(sy));
            const int fy = int((sy - y0) * 256);
            const uint8_t *r0 = y0 >= 0 ? s->rgb + size_t(y0) * s->w * 3 : nullptr;
            const uint8_t *r1 = y0 + 1 < s->h ? s->rgb + size_t(y0 + 1) * s->w * 3 : nullptr;
            const int left = int(e.cx) - s->w / 2;
            const int x0 = std::max(area.x0, left), x1 = std::min(area.x1, left + s->w);
            const float b = body_b * open;
            const bool lidded = shape.lid > 0.001f || std::fabs(shape.tilt) > 0.001f;
            for (int x = x0; x < x1; ++x) {
                const int sx = (x - left) * 3;
                int r = 0, g = 0, bl = 0;
                if (r0) { r += r0[sx] * (256 - fy); g += r0[sx + 1] * (256 - fy); bl += r0[sx + 2] * (256 - fy); }
                if (r1) { r += r1[sx] * fy; g += r1[sx + 1] * fy; bl += r1[sx + 2] * fy; }
                r >>= 8;
                g >>= 8;
                bl >>= 8;
                if (lidded) {
                    // A soft lid edge: the eyes are glowing light, not hard shapes.
                    const float raw = (x + 0.5f - e.cx) / body_a;
                    const float nx = std::max(-1.0f, std::min(1.0f, raw));
                    const float inner = e.left ? nx : -nx;
                    const float top = -b + shape.lid * 2 * b + shape.tilt * inner * b;
                    // Beside the eye the glow is left alone, so the lid stays on the eye.
                    const float side = smooth((std::fabs(raw) - 0.7f) / 0.6f);
                    const int kk = int(std::max(side, smooth((dy - top + 3) / 9.0f)) * 256);
                    r = (r * kk) >> 8;
                    g = (g * kk) >> 8;
                    bl = (bl * kk) >> 8;
                }
                uint8_t *o = line + (x - area.x0) * 3;
                o[0] = uint8_t(o[0] + r - o[0] * r / 255);
                o[1] = uint8_t(o[1] + g - o[1] * g / 255);
                o[2] = uint8_t(o[2] + bl - o[2] * bl / 255);
            }
        }
        uint16_t *out = c.row(y);
        for (int x = clip0; x < clip1; ++x) {
            const uint8_t *o = line + (x - area.x0) * 3;
            out[x] = pack_dithered(x, y, o[0], o[1], o[2]);
        }
    }
    return true;
}

// ---- Motion ------------------------------------------------------------------------

namespace {

EyeShape lerp(const EyeShape &a, const EyeShape &b, float t)
{
    return {a.open + (b.open - a.open) * t, a.lid + (b.lid - a.lid) * t, a.tilt + (b.tilt - a.tilt) * t,
            t < 0.5f ? a.happy : b.happy};
}
EyePose lerp(const EyePose &a, const EyePose &b, float t)
{
    EyePose p;
    p.left = lerp(a.left, b.left, t);
    p.right = lerp(a.right, b.right, t);
    p.gaze_x = a.gaze_x + (b.gaze_x - a.gaze_x) * t;
    p.gaze_y = a.gaze_y + (b.gaze_y - a.gaze_y) * t;
    p.lift = a.lift + (b.lift - a.lift) * t;
    return p;
}

/* Lid closure of a blink: 70 ms close, 30 ms shut, 120 ms open. */
float blink_closure(int64_t t)
{
    if (t < 0 || t >= 220) return 0;
    if (t < 70) return smooth(t / 70.0f);
    if (t < 100) return 1;
    return 1 - smooth((t - 100) / 120.0f);
}

} // namespace

uint32_t EyeMotion::random()
{
    seed_ ^= seed_ << 13;
    seed_ ^= seed_ >> 17;
    seed_ ^= seed_ << 5;
    return seed_;
}

void EyeMotion::reset(const EyePose &base, int64_t now_ms)
{
    from_ = to_ = base;
    changed_at_ = now_ms;
    ease_ms_ = 1;
    next_blink_ = now_ms + 1600;
    next_glance_ = now_ms + 2200;
    glance_from_x_ = glance_from_y_ = glance_x_ = glance_y_ = 0;
}

void EyeMotion::set(const EyePose &base, int64_t now_ms, int ease_ms)
{
    from_ = lerp(from_, to_, smooth(float(now_ms - changed_at_) / ease_ms_));
    to_ = base;
    changed_at_ = now_ms;
    ease_ms_ = std::max(1, ease_ms);
}

EyePose EyeMotion::pose(int64_t now_ms)
{
    EyePose p = lerp(from_, to_, smooth(float(now_ms - changed_at_) / ease_ms_));
    if (now_ms >= next_blink_) {
        blink_at_ = now_ms;
        // Mostly single blinks every 2.8-6 s, sometimes a quick double.
        next_blink_ = random() % 8 == 0 ? now_ms + 280 : now_ms + 2800 + random() % 3200;
    }
    const float closed = blink_closure(now_ms - blink_at_);
    p.left.open *= 1 - 0.94f * closed;
    p.right.open *= 1 - 0.94f * closed;

    float gx, gy;
    if (looking_) {
        gx = look_x_ * kMaxGaze;
        gy = look_y_ * kMaxGazeY;
    } else {
        if (glances && now_ms >= next_glance_) {
            const float t = smooth((now_ms - glance_at_) / 150.0f);
            glance_from_x_ += (glance_x_ - glance_from_x_) * t;
            glance_from_y_ += (glance_y_ - glance_from_y_) * t;
            // Small glances, back to centre about a third of the time.
            const bool centre = random() % 3 == 0;
            glance_x_ = centre ? 0 : (int(random() % 15) - 7);
            glance_y_ = centre ? 0 : (int(random() % 7) - 3);
            glance_at_ = now_ms;
            next_glance_ = now_ms + 2600 + random() % 2800;
        }
        const float t = smooth((now_ms - glance_at_) / 150.0f);
        gx = glance_from_x_ + (glance_x_ - glance_from_x_) * t;
        gy = glance_from_y_ + (glance_y_ - glance_from_y_) * t;
    }
    p.gaze_x += gx;
    p.gaze_y += gy;
    return p;
}

} // namespace fola
