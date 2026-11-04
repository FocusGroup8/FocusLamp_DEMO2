#include "task_guide_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace fola;

namespace task_guide {

namespace {

// ---- Palette (Figma R04 focus island 5:3735, screen states 5:732) -----------------
constexpr Rgb kTitle = hex(0x8de1e7);      // node core / teal accent
constexpr Rgb kText = hex(0xf3f8f9);       // primary step text
constexpr Rgb kQueuedText = hex(0xcbd6d8); // queued step text
constexpr Rgb kMuted = hex(0xa9b8bb);      // queued node digit, secondary text
constexpr Rgb kHint = hex(0x7d8b8e);
constexpr Rgb kNodeDigit = hex(0x092127);
constexpr Rgb kQueuedFill = hex(0xc7d7da);
constexpr Rgb kIslandTop = hex(0xb6f5f6), kIslandBottom = hex(0x4ec3cf), kIslandStroke = hex(0xb8f5f6);
constexpr Rgb kIslandSpill = hex(0x56cdd7), kNodeSpill = hex(0x69d5de);
constexpr Rgb kPill = hex(0x6cc0c7), kPillGlow = hex(0x8ee0e6); // TASK START light well
constexpr Rgb kDoneText = hex(0xe6dbef);

// ---- Layout, design px (480 space). Figma boxes are converted with fig_*(). --------
// Preview: one lit "focus island" for the first step, two quiet queued islands.
const float kIslandX = fig_x(88), kIslandY = fig_y(122), kIslandW = fig_len(354), kIslandH = fig_len(117);
const float kIslandR = fig_len(40);
const float kQueuedX[2] = {fig_x(100), fig_x(99)}, kQueuedY[2] = {fig_y(255), fig_y(334)};
const float kQueuedW[2] = {fig_len(330), fig_len(333)}, kQueuedH = fig_len(66), kQueuedR = fig_len(27);
const float kNodeX = fig_x(131), kNodeR = fig_len(19);
const float kStepTextX = fig_x(165);

// Running: eyes above, current step below, track around the edge. The eye
// repaint rectangle (eyes_area) must not reach the text or the track band.
const EyeLayout kRunningEyes = {&kGreen, 0.70f, 240, 168, true};
constexpr float kRunTextY1 = 292, kRunTextY2[2] = {282, 320}, kRunTextWidth = 300;
constexpr float kPillsY = 362, kTimeY = 374, kHintY = 404;

const EyeLayout kDoneEyes = {&kLavender, 0.85f, 240, 196};
const EyeLayout kNoticeEyes = {&kTeal, 0.80f, 240, 180};

constexpr float kTextScale = 2.0f, kSmallScale = 1.5f;

Track &track()
{
    static Track shared;
    return shared;
}

int track_level(uint32_t remaining_ms)
{
    // 2048 levels: roughly one level per track pixel, about every 90 ms.
    return int(uint64_t(remaining_ms) * 2048 / Model::step_ms);
}

float remaining_fraction(uint32_t remaining_ms) { return float(remaining_ms) / Model::step_ms; }

void centred_block(const Canvas &c, const char *utf8, float scale, float max_width, float centre_y,
                   float line_gap, float x, bool centre_x, Rgb color)
{
    char lines[2][kLineBytes];
    const int count = wrap_two_lines(utf8, scale, max_width, lines);
    const float height = 16 * scale, total = count * height + (count - 1) * line_gap;
    for (int i = 0; i < count; ++i) {
        const float y = centre_y - total / 2 + i * (height + line_gap);
        if (centre_x) text_centered(c, x, y, lines[i], scale, color);
        else text(c, x, y, lines[i], scale, color);
    }
}

void step_node(const Canvas &c, float cy, int number, bool lit)
{
    char digit[2] = {char('0' + number), 0};
    if (lit) {
        circle_glow(c, kNodeX, cy, kNodeR, 8, kNodeSpill, 0.35f);
        circle(c, kNodeX, cy, kNodeR, kTitle, 1);
    } else {
        circle(c, kNodeX, cy, kNodeR, kQueuedFill, 0.10f);
    }
    text_centered(c, kNodeX, cy - 12, digit, kSmallScale, lit ? kNodeDigit : kMuted);
}

void draw_preview(const Canvas &c, const View &v)
{
    text_centered(c, 240, 42, "三步任务", kSmallScale, kTitle);
    round_rect_glow(c, fig_x(97), fig_y(109), fig_len(336), fig_len(124), fig_len(44), 18, kIslandSpill, 0.18f);
    round_rect_gradient(c, kIslandX, kIslandY, kIslandW, kIslandH, kIslandR, kIslandTop, 0.15f, kIslandBottom, 0.05f);
    round_rect_outline(c, kIslandX, kIslandY, kIslandW, kIslandH, kIslandR, kIslandStroke, 0.14f);
    const float island_cy = kIslandY + kIslandH / 2;
    step_node(c, island_cy, 1, true);
    centred_block(c, v.steps[0], kTextScale, kIslandX + kIslandW - 22 - kStepTextX, island_cy, 6, kStepTextX,
                  false, kText);
    for (int i = 0; i < 2; ++i) {
        round_rect(c, kQueuedX[i], kQueuedY[i], kQueuedW[i], kQueuedH, kQueuedR, kQueuedFill, 0.05f);
        const float cy = kQueuedY[i] + kQueuedH / 2;
        step_node(c, cy, i + 2, false);
        centred_block(c, v.steps[i + 1], kSmallScale, kQueuedX[i] + kQueuedW[i] - 18 - kStepTextX, cy, 4,
                      kStepTextX, false, kQueuedText);
    }
}

void draw_banner(const Canvas &c, unsigned step)
{
    // Figma TASK START (5:6911): a glowing teal capsule across the middle.
    const float x = fig_x(95), y = fig_y(210), w = fig_len(340), h = fig_len(88);
    round_rect_glow(c, x, y, w, h, h / 2, 14, kPillGlow, 0.55f);
    round_rect(c, x, y, w, h, h / 2, kPill, 1);
    char label[24];
    std::snprintf(label, sizeof(label), "第%u步", step + 1);
    text_centered(c, 240, y + h / 2 - 20, label, 2.5f, kText);
}

void draw_step_pills(const Canvas &c, unsigned step)
{
    constexpr float w = 26, h = 6, gap = 10;
    const float x0 = 240 - (3 * w + 2 * gap) / 2;
    for (unsigned i = 0; i < TG_STEP_COUNT; ++i) {
        const float x = x0 + i * (w + gap);
        if (i < step) round_rect(c, x, kPillsY, w, h, h / 2, kGreenTrack.mid, 0.55f);
        else if (i == step) round_rect(c, x, kPillsY, w, h, h / 2, kGreenTrack.end, 1);
        else round_rect(c, x, kPillsY, w, h, h / 2, kGreenTrack.groove, 1);
    }
}

EyePose worried_pose()
{
    EyePose p;
    p.left.tilt = p.right.tilt = -0.22f;
    p.left.lid = p.right.lid = 0.04f;
    p.gaze_y = 3;
    return p;
}

EyePose happy_pose()
{
    EyePose p;
    p.left.happy = p.right.happy = true;
    return p;
}

} // namespace

void Screen::prepare(const Canvas &canvas)
{
    prepare_eyes(canvas, kRunningEyes);
    prepare_eyes(canvas, kDoneEyes, true);
    prepare_eyes(canvas, kNoticeEyes);
    track().prepare(canvas);
}

void Screen::release()
{
    track().release();
    release_eye_cache();
    scene_ = Scene::None;
}

Screen::Scene Screen::scene_for(const Frame &f)
{
    switch (f.view.state) {
    case State::Preview: return Scene::Preview;
    case State::Done: return Scene::Done;
    case State::Running:
        return Model::step_ms - f.view.remaining_ms < kBannerMs ? Scene::Banner : Scene::Running;
    case State::Idle: break;
    }
    return f.notice ? Scene::Notice : Scene::None;
}

void Screen::draw(const Canvas &c, const Frame &frame, int64_t now_ms, bool continuing)
{
    const Scene scene = scene_for(frame);
    if (scene == Scene::None) {
        scene_ = Scene::None;
        return;
    }
    const View &v = frame.view;
    const bool same = continuing && scene == scene_ && v.controller_boot_id == boot_ && v.request_id == request_ &&
        v.step == step_;
    if (!same) {
        if (scene != scene_ || !continuing) scene_started_ms_ = now_ms;
        scene_ = scene;
        boot_ = v.controller_boot_id;
        request_ = v.request_id;
        step_ = v.step;
        draw_full(c, scene, frame, now_ms);
        return;
    }
    // Incremental updates for animated scenes; the rest is static.
    switch (scene) {
    case Scene::Running: {
        const int level = track_level(v.remaining_ms);
        if (level != track_level_) {
            track_level_ = level;
            track().draw(c, remaining_fraction(v.remaining_ms), kGreenTrack);
        }
        draw_running_time(c, v.remaining_ms, false);
        draw_eyes(c, kRunningEyes, eyes_.pose(now_ms));
        break;
    }
    case Scene::Banner: {
        const int level = track_level(v.remaining_ms);
        if (level != track_level_) {
            track_level_ = level;
            track().draw(c, remaining_fraction(v.remaining_ms), kGreenTrack);
        }
        break;
    }
    case Scene::Done: draw_done_eyes(c, now_ms); break;
    case Scene::Notice: draw_eyes(c, kNoticeEyes, eyes_.pose(now_ms)); break;
    default: break;
    }
}

void Screen::draw_full(const Canvas &c, Scene scene, const Frame &frame, int64_t now_ms)
{
    const View &v = frame.view;
    c.background(c.bounds());
    track_level_ = -1;
    seconds_ = ~0u;
    switch (scene) {
    case Scene::Preview: draw_preview(c, v); break;
    case Scene::Banner:
        track_level_ = track_level(v.remaining_ms);
        track().draw(c, remaining_fraction(v.remaining_ms), kGreenTrack);
        draw_banner(c, v.step);
        break;
    case Scene::Running: {
        track_level_ = track_level(v.remaining_ms);
        track().draw(c, remaining_fraction(v.remaining_ms), kGreenTrack);
        eyes_.reset(EyePose{}, now_ms);
        eyes_.glances = true;
        draw_eyes(c, kRunningEyes, eyes_.pose(now_ms));
        char lines[2][kLineBytes];
        const int count = wrap_two_lines(v.steps[v.step], kTextScale, kRunTextWidth, lines);
        for (int i = 0; i < count; ++i)
            text_centered(c, 240, count == 1 ? kRunTextY1 : kRunTextY2[i], lines[i], kTextScale, kText);
        draw_step_pills(c, v.step);
        draw_running_time(c, v.remaining_ms, true);
        text_centered(c, 240, kHintY, "点击退出", 1.0f, kHint);
        break;
    }
    case Scene::Done:
        eyes_.reset(happy_pose(), now_ms);
        eyes_.glances = false;
        draw_done_eyes(c, now_ms);
        text_centered(c, 240, 342, "本轮引导结束", kSmallScale, kDoneText);
        break;
    case Scene::Notice:
        eyes_.reset(worried_pose(), now_ms);
        eyes_.glances = false;
        draw_eyes(c, kNoticeEyes, eyes_.pose(now_ms));
        text_centered(c, 240, 318, "暂时没收到结果", kTextScale, kText);
        text_centered(c, 240, 360, "再说一次试试", kSmallScale, kMuted);
        break;
    case Scene::None: break;
    }
}

void Screen::draw_running_time(const Canvas &c, uint32_t remaining_ms, bool repaint)
{
    const unsigned seconds = (remaining_ms + 999) / 1000;
    if (!repaint && seconds == seconds_) return;
    seconds_ = seconds;
    char label[16];
    std::snprintf(label, sizeof(label), "%u:%02u", seconds / 60, seconds % 60);
    if (!repaint) c.background(c.to_pixels(180, kTimeY, 120, 16 * kSmallScale));
    text_centered(c, 240, kTimeY, label, kSmallScale, kQueuedText, 0.85f);
}

void Screen::draw_done_eyes(const Canvas &c, int64_t now_ms)
{
    // A small double hop, then rest, like Figma's completion (5:815).
    const float t = float(now_ms - scene_started_ms_);
    EyePose pose = eyes_.pose(now_ms);
    if (t < 700) pose.lift = -kMaxLift * std::fabs(std::sin(t / 350.0f * 3.1415927f));
    draw_eyes(c, kDoneEyes, pose);
    const float twinkle = 0.55f + 0.45f * std::sin(t / 260.0f);
    sparkle(c, 88, 150, 16, kDoneText, twinkle);
    sparkle(c, 396, 236, 12, kDoneText, 1.1f - twinkle);
}

} // namespace task_guide
