#include "fola_face.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace fola {
namespace {
constexpr float kHalfWidth = 52.5f, kHalfHeight = 74.f;
constexpr float kTileHalfWidth = 74.f, kTileHalfHeight = 96.f;
constexpr float kCentreY = 222.f, kHalfSpacing = 88.f;
enum Shape { Normal, Smile, Sad, Firm, Concave };
const char *const kLabels[] = {"neutral", "happy", "sad", "angry", "surprised", "sleepy", "bored",
                              "wink_left", "wink_right", "thinking", "confused", "listening", "speaking"};
float clamp(float value, float lo, float hi) { return std::max(lo, std::min(hi, value)); }
float smooth(float value) { value = clamp(value, 0, 1); return value * value * (3 - 2 * value); }
float mix(float a, float b, float t) { return a + (b - a) * t; }
float edge(float distance, float radius) { return 1.f / (1.f + std::exp(1.702f * distance / radius)); }
int left_shape(FaceLabel label)
{
    if (label == FaceLabel::Happy) return Smile;
    if (label == FaceLabel::Sad) return Sad;
    if (label == FaceLabel::Angry) return Firm;
    return Normal;
}
int right_shape(FaceLabel label)
{
    return label == FaceLabel::Confused ? Concave : left_shape(label);
}
// Distance to a superellipse whose upper/lower boundaries carry the reference
// expression outline. This keeps the nearby glow attached to the shaped edge.
float shape_distance(float x, float y, float hw, float hh, int shape)
{
    const float nx = clamp(x / hw, -1, 1);
    const float centre_weight = (1 - nx * nx) * (1 - nx * nx);
    float top = -hh, bottom = hh;
    if (shape == Smile) bottom -= 24.f * centre_weight;
    if (shape == Sad) top -= .34f * x;
    if (shape == Firm) top += .38f * x;
    if (shape == Concave) { top += 19.f * centre_weight; bottom -= 19.f * centre_weight; }
    const float cy = (top + bottom) * .5f, height = (bottom - top) * .5f;
    const float u = x / hw, v = (y - cy) / height;
    const float f = u * u * u * u + v * v * v * v;
    if (f < 1e-10f) return -std::min(hw, height);
    return std::hypot(x, y - cy) * (1 - 1 / std::sqrt(std::sqrt(f)));
}
// Muted teal from the supplied expression-library preview. There is no broad
// ambient-spill layer; only the glow immediately surrounding the eye remains.
void render_tile(uint8_t *out, int w, int h, float scale, int shape)
{
    const Rgb outer = hex(0x73aba7), centre = hex(0xb9dcd3), sheen = hex(0xc9e3da);
    for (int j = 0; j < h; ++j) {
        const float y = (j + .5f - h * .5f) / scale;
        for (int i = 0; i < w; ++i, out += 3) {
            const float x = (i + .5f - w * .5f) / scale;
            const float distance = shape_distance(x, y, kHalfWidth, kHalfHeight, shape);
            const float body_alpha = edge(distance, .9f);
            const float glow_alpha = .23f * edge(distance, 4.4f);
            const float radial = clamp(1 - std::hypot((x + 3) / 80.f, (y + 9) / 112.f), 0, 1);
            const float highlight = .14f * edge(shape_distance(x + 2, y + 7, 30, 47, shape), 7);
            const int a[3] = {outer.r, outer.g, outer.b}, b[3] = {centre.r, centre.g, centre.b};
            const int hi[3] = {sheen.r, sheen.g, sheen.b};
            for (int channel = 0; channel < 3; ++channel) {
                const float color = mix(float(a[channel]), float(b[channel]), smooth(radial));
                float value = color * body_alpha;
                value = value + a[channel] * glow_alpha * (1 - value / 255.f);
                value = mix(value, float(hi[channel]), highlight * body_alpha);
                out[channel] = uint8_t(clamp(value + .5f, 0, 255));
            }
        }
    }
}
float blink_closure(int64_t elapsed, int duration)
{
    if (elapsed < 0 || elapsed >= duration) return 0;
    const float phase = float(elapsed) / duration;
    if (phase < .35f) return smooth(phase / .35f);
    if (phase < .5f) return 1;
    return 1 - smooth((phase - .5f) / .5f);
}
} // namespace

bool face_label_from_name(const char *name, FaceLabel &label)
{
    if (!name) return false;
    for (int i = 0; i < 13; ++i) if (!std::strcmp(name, kLabels[i])) { label = static_cast<FaceLabel>(i); return true; }
    return false;
}
const char *face_label_name(FaceLabel label)
{
    const int value = static_cast<int>(label);
    return value >= 0 && value < 13 ? kLabels[value] : "neutral";
}
void FaceRenderer::reset(int64_t now_ms)
{
    label_ = FaceLabel::Neutral;
    entered_ms_ = now_ms;
    blink_at_ms_ = now_ms - blink_duration_ms_;
    next_blink_ms_ = now_ms + interval_ms_;
    gaze_x_ = gaze_y_ = 0;
}
void FaceRenderer::set_label(FaceLabel label, int64_t now_ms)
{
    const int value = static_cast<int>(label);
    if (value < 0 || value >= 13) label = FaceLabel::Neutral;
    if (label == label_) return;
    label_ = label;
    entered_ms_ = now_ms;
    // A new expression gets a readable first frame, rather than inheriting a blink.
    blink_at_ms_ = now_ms - blink_duration_ms_;
    next_blink_ms_ = now_ms + interval_ms_;
}
void FaceRenderer::look_at(float x, float y)
{
    gaze_x_ = clamp(std::isfinite(x) ? x : 0, -1, 1) * 10;
    gaze_y_ = clamp(std::isfinite(y) ? y : 0, -1, 1) * 6;
}
void FaceRenderer::blink(int64_t now_ms) { blink_at_ms_ = now_ms; }
void FaceRenderer::configure_auto_blink(bool enabled, float interval_seconds, float duration_seconds)
{
    auto_blink_ = enabled;
    interval_ms_ = int(clamp(std::isfinite(interval_seconds) ? interval_seconds : 4, .5f, 60) * 1000);
    blink_duration_ms_ = int(clamp(std::isfinite(duration_seconds) ? duration_seconds : .12f, .08f, 1) * 1000);
    next_blink_ms_ = entered_ms_ + interval_ms_;
}
FaceFrame FaceRenderer::frame(int64_t now_ms)
{
    FaceFrame result;
    const float t = std::max<int64_t>(0, now_ms - entered_ms_) / 1000.f;
    const float breath = std::sin(t * 1.6f);
    result.gaze_x = gaze_x_;
    result.gaze_y = gaze_y_;
    switch (label_) {
    case FaceLabel::Happy: result.left_open = result.right_open = .9f; result.lift = -2; break;
    case FaceLabel::Sad: result.left_open = result.right_open = .92f; result.lift = 3; break;
    case FaceLabel::Angry: result.left_open = result.right_open = .9f; break;
    case FaceLabel::Surprised:
        result.left_open = result.right_open = 1.08f + .10f * std::exp(-t * 7.f) * std::sin(t * 15.f + 1.f);
        result.lift = -3; break;
    case FaceLabel::Sleepy: result.left_open = result.right_open = .39f + .015f * breath; result.lift = 5; break;
    case FaceLabel::Bored: result.left_open = result.right_open = .82f; result.gaze_x -= 5; break;
    case FaceLabel::WinkLeft: result.right_open = .055f; break; // Robot left is the viewer's right.
    case FaceLabel::WinkRight: result.left_open = .055f; break;
    case FaceLabel::Thinking: result.gaze_x += 6 * std::sin(t * 1.25f); result.gaze_y -= 5; result.lift = -2; break;
    case FaceLabel::Confused: result.right_open = .58f; result.left_open = .94f; result.question = true; break;
    case FaceLabel::Listening: result.left_open = result.right_open = 1.02f + .025f * std::sin(t * 2.f); break;
    case FaceLabel::Speaking:
        // Syllable-like motion, not audio amplitude: no microphone/speaker dependency.
        result.left_open = result.right_open = .91f + .065f * std::sin(t * 12.f) + .025f * std::sin(t * 21.f);
        break;
    default: result.left_open = result.right_open = 1 + .008f * breath; break;
    }
    const bool wink = label_ == FaceLabel::WinkLeft || label_ == FaceLabel::WinkRight;
    if (auto_blink_ && !wink && now_ms >= next_blink_ms_) {
        blink_at_ms_ = now_ms;
        next_blink_ms_ = now_ms + interval_ms_;
    }
    const float closure = blink_closure(now_ms - blink_at_ms_, blink_duration_ms_);
    result.left_open *= 1 - .95f * closure;
    result.right_open *= 1 - .95f * closure;
    return result;
}
const FaceRenderer::Sprite *FaceRenderer::sprite_for(const Canvas &canvas, int shape)
{
    Sprite *slot = nullptr;
    for (Sprite &s : sprites_) {
        if (s.rgb && s.shape == shape && s.unit == canvas.unit()) { s.used = ++use_clock_; return &s; }
        if (!slot || !s.rgb || (slot->rgb && s.used < slot->used)) slot = &s;
    }
    const float k = kFigmaScale * canvas.unit();
    const int w = 2 * int(std::ceil(kTileHalfWidth * k)), h = 2 * int(std::ceil(kTileHalfHeight * k));
    const size_t bytes = size_t(w) * h * 3;
    if (!slot || !w || !h) return nullptr;
    if (slot->capacity < bytes) {
        std::free(slot->rgb);
        slot->rgb = static_cast<uint8_t *>(cache_alloc(bytes));
        slot->capacity = slot->rgb ? bytes : 0;
    }
    if (!slot->rgb) { slot->shape = -1; return nullptr; }
    slot->w = w; slot->h = h; slot->shape = shape; slot->unit = canvas.unit(); slot->used = ++use_clock_;
    render_tile(slot->rgb, w, h, k, shape);
    return slot;
}
void FaceRenderer::prepare(const Canvas &canvas)
{
    sprite_for(canvas, left_shape(label_));
    sprite_for(canvas, right_shape(label_));
}
bool FaceRenderer::draw(const Canvas &canvas, int64_t now_ms)
{
    canvas.background(canvas.bounds());
    const FaceFrame state = frame(now_ms);
    const Sprite *pair[2] = {sprite_for(canvas, left_shape(label_)), sprite_for(canvas, right_shape(label_))};
    if (!pair[0] || !pair[1]) return false;
    const float unit = canvas.unit();
    for (int eye = 0; eye < 2; ++eye) {
        const Sprite &sprite = *pair[eye];
        const float open = clamp(eye == 0 ? state.left_open : state.right_open, .04f, 1.2f);
        const float cx = (240 + (eye == 0 ? -kHalfSpacing : kHalfSpacing) + state.gaze_x) * unit;
        const float cy = (kCentreY + state.gaze_y + state.lift) * unit;
        const int x0 = int(std::round(cx)) - sprite.w / 2;
        const int y0 = std::max(0, int(std::floor(cy - sprite.h * open * .5f)));
        const int y1 = std::min(canvas.height(), int(std::ceil(cy + sprite.h * open * .5f)));
        for (int y = y0; y < y1; ++y) {
            const float sy = (y + .5f - cy) / open + sprite.h * .5f - .5f;
            const int j = int(std::floor(sy));
            const int fraction = int((sy - j) * 256);
            for (int x = std::max(0, x0); x < std::min(canvas.width(), x0 + sprite.w); ++x) {
                // Mirrored outlines: the Sad/Firm slopes point to the pair's centre.
                int column = x - x0;
                if (eye == 1) column = sprite.w - 1 - column;
                const uint8_t *a = j >= 0 && j < sprite.h ? sprite.rgb + (size_t(j) * sprite.w + column) * 3 : nullptr;
                const uint8_t *b = j + 1 >= 0 && j + 1 < sprite.h ? sprite.rgb + (size_t(j + 1) * sprite.w + column) * 3 : nullptr;
                int color[3] = {};
                for (int c = 0; c < 3; ++c) color[c] = ((a ? a[c] * (256 - fraction) : 0) + (b ? b[c] * fraction : 0)) >> 8;
                if (color[0] + color[1] + color[2]) canvas.screen(x, y, {uint8_t(color[0]), uint8_t(color[1]), uint8_t(color[2])}, 256);
            }
        }
    }
    if (state.question) text_centered(canvas, 424, 122, "?", 2.7f, hex(0xb6a1cc));
    return true;
}
void FaceRenderer::release()
{
    for (Sprite &sprite : sprites_) { std::free(sprite.rgb); sprite = {}; }
    use_clock_ = 0;
}
size_t FaceRenderer::cache_bytes() const
{
    size_t bytes = 0;
    for (const Sprite &sprite : sprites_) bytes += sprite.capacity;
    return bytes;
}
} // namespace fola
