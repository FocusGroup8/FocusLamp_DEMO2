#pragma once

// FOBI glow eyes from Figma 台灯 / 眼睛 (5:49): neutral teal pair 5:2797,
// green focus pair 5:5293, lavender / lemon / orange spectrum variants.
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
extern const Palette kLemon;    // 明亮柠檬黄 Lemon Yellow
extern const Palette kOrange;   // 橘子汽水 Mirinda Orange

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
/* Figma neutral placement: full size, slightly above the panel centre. */
EyeLayout neutral_layout(const Palette &palette);

/* Largest gaze/lift a pose may use; the repaint rectangle covers it. */
constexpr float kMaxGaze = 10, kMaxGazeY = 6, kMaxLift = 8;

/* Rectangle repainted by draw_eyes for this layout (background included). */
Rect eyes_area(const Canvas &c, const EyeLayout &layout);
/* Repaints eyes_area: background, then both eyes. Returns false (and paints
 * only background) if the sprite cache could not be allocated. */
bool draw_eyes(const Canvas &c, const EyeLayout &layout, const EyePose &pose);
/* Builds the sprite ahead of time (it takes tens of ms on the first use). */
void prepare_eyes(const Canvas &c, const EyeLayout &layout, bool happy = false);

/* Blinks, small glances and eased expression changes, driven by now_ms. */
class EyeMotion {
public:
    void reset(const EyePose &base, int64_t now_ms);
    void set(const EyePose &base, int64_t now_ms, int ease_ms = 180);
    void blink(int64_t now_ms) { blink_at_ = now_ms; }
    /* External gaze in -1..1; disables the automatic glances until cleared. */
    void look(float x, float y) { look_x_ = x; look_y_ = y; looking_ = true; }
    void clear_look() { looking_ = false; }
    bool glances = true;
    EyePose pose(int64_t now_ms);

private:
    uint32_t random();
    EyePose from_, to_;
    int64_t changed_at_ = 0, ease_ms_ = 1, blink_at_ = -100000, next_blink_ = 0;
    int64_t glance_at_ = 0, next_glance_ = 0;
    float glance_from_x_ = 0, glance_from_y_ = 0, glance_x_ = 0, glance_y_ = 0;
    float look_x_ = 0, look_y_ = 0;
    bool looking_ = false;
    uint32_t seed_ = 0x2f6b9d1u;
};

} // namespace fola
