#pragma once

// FOBI glow eyes from Figma 台灯 / 眼睛 (5:49): neutral teal pair 5:2797,
// green task timer and lavender completion eyes.
//
// Each eye is seven concentric superellipse layers (spill, two blooms, body,
// core glow, core, highlight). They are composited once per palette and
// scale into a PSRAM sprite; a frame only blits the sprite with blink,
// eyelid, arch (happy) and gaze applied, so per-frame cost is one pass over
// the eye rectangle.

#include "fola_gfx.h"

#include <cstdint>

namespace fola {

struct Palette {
    Rgb spill, bloom_wide, bloom_tight;
    Rgb body[3];   // radial stops 0, 0.54, 1
    Rgb core_glow;
    Rgb core[4];   // radial stops 0, 0.48, 0.82, 1
    Rgb highlight;
};
extern const Palette kTeal;     // 孔雀石青 Peacock Teal (P12)
extern const Palette kGreen;    // 草芽嫩绿 Tender Grass (focus progress)
extern const Palette kLavender; // 薰衣草雾 Lavender Haze (completion)
/* Per-eye shape. The defaults are the Figma neutral eye. Lids are cut at
 * draw time; the happy arch is its own sprite so the glow follows it. */
struct EyeShape {
    float open = 1;  // vertical scale; blink and squint (休眠眯眯眼) go towards 0
    float lid = 0;   // top lid lowered by this fraction of the eye height
    float tilt = 0;  // lid slope: >0 inner corner lower (firm), <0 inner higher (worried)
    bool happy = false; // bottom pushed up into an arch (completion eyes, 5:815)
};

struct EyePose {
    EyeShape left, right;
    float gaze_x = 0, gaze_y = 0; // design px
    float lift = 0;               // whole pair vertical offset, design px
};

struct EyeLayout {
    const Palette *palette;
    float scale;   // 1 = Figma size (body 105x148 Figma px)
    float cx, cy;  // centre between the eyes, design space
    bool inside_track = false; // clip the repaint to the inside of fola::Track
};
/* Largest gaze/lift a pose may use; the repaint rectangle covers it. */
constexpr float kMaxGaze = 10, kMaxGazeY = 6, kMaxLift = 8;

/* Rectangle repainted by draw_eyes for this layout (background included). */
Rect eyes_area(const Canvas &c, const EyeLayout &layout);
/* Repaints eyes_area: background, then both eyes. Returns false (and paints
 * only background) if the sprite cache could not be allocated. */
bool draw_eyes(const Canvas &c, const EyeLayout &layout, const EyePose &pose);
/* Builds the sprite ahead of time (it takes tens of ms on the first use). */
void prepare_eyes(const Canvas &c, const EyeLayout &layout, bool happy = false);
void release_eye_cache();

/* Blinks and small task-screen glances, driven by now_ms. */
class EyeMotion {
public:
    void reset(const EyePose &base, int64_t now_ms);
    bool glances = true;
    EyePose pose(int64_t now_ms);

private:
    uint32_t random();
    EyePose base_;
    int64_t blink_at_ = -100000, next_blink_ = 0;
    int64_t glance_at_ = 0, next_glance_ = 0;
    float glance_from_x_ = 0, glance_from_y_ = 0, glance_x_ = 0, glance_y_ = 0;
    uint32_t seed_ = 0x2f6b9d1u;
};

} // namespace fola
