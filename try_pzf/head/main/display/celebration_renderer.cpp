#include "celebration_renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace fola {
namespace {

struct Art {
    int x, y, width, height;
    const uint32_t *rows;
    const uint8_t *runs;
};
#include "celebration_art.inc"

constexpr int kPopMs = 420;
constexpr float kScale = 480.f / 374.f;
constexpr float kTrophyX = (800 - 613) * kScale;
constexpr float kTrophyY = (985 - 753) * kScale;
constexpr Rgb kTrophyColor{185, 164, 204};
constexpr Rgb kStarColor{203, 184, 219};

float smooth(float value)
{
    const float t = std::clamp(value, 0.f, 1.f);
    return t * t * (3.f - 2.f*t);
}

void background(const Canvas &canvas, Rect box)
{
    // The approved glass gradient is separable, so there is no exp per pixel.
    const float unit = canvas.unit();
    std::array<float, 480> horizontal;
    for (int x = 0; x < 480; ++x) {
        const float u = (x - 42.f) / 325.f;
        horizontal[x] = std::exp(-u*u);
    }
    for (int y = box.y0; y < box.y1; ++y) {
        const float v = (y / unit - 22.f) / 233.f;
        const float vertical = std::exp(-v*v);
        for (int x = box.x0; x < box.x1; ++x) {
            const float sheen = vertical * horizontal[std::clamp(int(x/unit), 0, 479)];
            canvas.write(x, y, {uint8_t(3.5f + 31.f*sheen), uint8_t(6.5f + 34.f*sheen),
                                 uint8_t(9.5f + 39.f*sheen)});
        }
    }
}

void draw_eye(const Canvas &canvas, const Art &eye)
{
    const auto rect = canvas.to_pixels(eye.x, eye.y, eye.width, eye.height);
    const float unit = canvas.unit();
    std::array<uint16_t, kLeftEye.width> row;
    static_assert(kLeftEye.width >= kRightEye.width);
    for (int y = rect.y0; y < rect.y1; ++y) {
        const int sy = std::clamp(int((y+.5f)/unit)-eye.y, 0, eye.height-1);
        const uint8_t *run = eye.runs + eye.rows[sy];
        for (int x = 0; x < eye.width; run += 3) {
            const uint16_t color = run[1] | uint16_t(run[2]) << 8;
            std::fill_n(row.data()+x, run[0], color);
            x += run[0];
        }
        auto *dst = canvas.row(y);
        if (unit == 1.f) {
            std::memcpy(dst+rect.x0, row.data()+rect.x0-eye.x, (rect.x1-rect.x0)*sizeof(uint16_t));
        } else {
            for (int x = rect.x0; x < rect.x1; ++x)
                dst[x] = row[std::clamp(int((x+.5f)/unit)-eye.x, 0, eye.width-1)];
        }
    }
}

using MaskRow = std::array<uint8_t, kTrophy.width>;
void read_trophy_row(int y, MaskRow &row)
{
    if (y < 0 || y >= kTrophy.height) {
        row.fill(0);
        return;
    }
    const uint8_t *run = kTrophy.runs + kTrophy.rows[y];
    for (int x = 0; x < kTrophy.width; run += 2) {
        std::fill_n(row.data()+x, run[0], run[1]);
        x += run[0];
    }
}

void draw_trophy(const Canvas &canvas, int ms)
{
    const float t = ms / float(kPopMs), r = t-1.f;
    const float pop = .45f + .55f*(1.f + 2.4f*r*r*r + 1.4f*r*r);
    const float opacity = smooth(ms/110.f);
    if (opacity == 0.f) return;
    const float left = kTrophyX + (kTrophy.x-kTrophyX)*pop;
    const float top = kTrophyY + (kTrophy.y-kTrophyY)*pop + 13.f*kScale*(1.f-t);
    const auto rect = canvas.to_pixels(left, top, kTrophy.width*pop, kTrophy.height*pop);
    const float unit = canvas.unit();
    MaskRow above, below;
    const auto at = [](const MaskRow &row, int x) {
        return x >= 0 && x < kTrophy.width ? float(row[x]) : 0.f;
    };
    for (int y = rect.y0; y < rect.y1; ++y) {
        const float sy = ((y+.5f)/unit-top)/pop-.5f;
        const int yi = int(std::floor(sy));
        const float fy = sy-yi;
        read_trophy_row(yi, above);
        read_trophy_row(yi+1, below);
        for (int x = rect.x0; x < rect.x1; ++x) {
            const float sx = ((x+.5f)/unit-left)/pop-.5f;
            const int xi = int(std::floor(sx));
            const float fx = sx-xi;
            const float a = (1.f-fx)*at(above,xi) + fx*at(above,xi+1);
            const float b = (1.f-fx)*at(below,xi) + fx*at(below,xi+1);
            canvas.blend(x,y,kTrophyColor,int(((1.f-fy)*a+fy*b)*opacity*(256.f/255.f)));
        }
    }
    sparkle(canvas, (673-613)*kScale, (945-753)*kScale, 17*kScale, kStarColor, opacity);
    sparkle(canvas, (927-613)*kScale, (929-753)*kScale, 15*kScale, kStarColor, opacity);
}

} // namespace

void Celebration::draw(const Canvas &canvas, int64_t elapsed_ms, bool repaint)
{
    if (canvas.unit() <= 0) return;
    const int ms = int(std::clamp<int64_t>(elapsed_ms, 0, kPopMs));
    if (!repaint && painted_ms_ == ms) return;
    if (repaint || painted_ms_ < 0) {
        background(canvas, canvas.bounds());
        draw_eye(canvas, kLeftEye);
        draw_eye(canvas, kRightEye);
    } else {
        // Redraw only the area used by the old and new position of the trophy.
        background(canvas, canvas.to_pixels(36, 185, 408, 240));
    }
    draw_trophy(canvas, ms);
    painted_ms_ = ms;
}

} // namespace fola
