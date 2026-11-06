#pragma once

// Default FOLA face. Independent of LVGL/FreeRTOS so previews use the same renderer.
// Draws into the caller's existing RGB565 Canvas; no full-screen allocation.
#include "fola_gfx.h"

#include <cstddef>
#include <cstdint>

namespace fola {

// The first nine values deliberately match expressive_eyes_expression_t.
enum class FaceLabel {
    Neutral, Happy, Sad, Angry, Surprised, Sleepy, Bored, WinkLeft, WinkRight,
    Thinking, Confused, Listening, Speaking
};
bool face_label_from_name(const char *name, FaceLabel &label);
const char *face_label_name(FaceLabel label);

struct FaceFrame {
    float left_open = 1, right_open = 1;
    float gaze_x = 0, gaze_y = 0, lift = 0;
    bool question = false;
};

class FaceRenderer {
public:
    FaceRenderer() = default;
    ~FaceRenderer() { release(); }
    FaceRenderer(const FaceRenderer &) = delete;
    FaceRenderer &operator=(const FaceRenderer &) = delete;
    void reset(int64_t now_ms);
    void set_label(FaceLabel label, int64_t now_ms);
    void look_at(float x, float y);
    void blink(int64_t now_ms);
    void configure_auto_blink(bool enabled, float interval_seconds, float duration_seconds = .12f);
    // Prepares only the current pair, not the complete expression library.
    void prepare(const Canvas &canvas);
    bool draw(const Canvas &canvas, int64_t now_ms);
    FaceFrame frame(int64_t now_ms);
    void release();
    size_t cache_bytes() const;

private:
    struct Sprite {
        uint8_t *rgb = nullptr;
        size_t capacity = 0;
        int w = 0, h = 0, shape = -1;
        float unit = 0;
        uint32_t used = 0;
    };
    const Sprite *sprite_for(const Canvas &canvas, int shape);
    FaceLabel label_ = FaceLabel::Neutral;
    int64_t entered_ms_ = 0, blink_at_ms_ = -10000, next_blink_ms_ = 4000;
    float gaze_x_ = 0, gaze_y_ = 0;
    bool auto_blink_ = true;
    int interval_ms_ = 4000, blink_duration_ms_ = 120;
    uint32_t use_clock_ = 0;
    Sprite sprites_[2];
};

} // namespace fola
