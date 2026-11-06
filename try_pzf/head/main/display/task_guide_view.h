#pragma once

// Task-guide screens. Pure drawing: timing, touch and reporting stay in
// task_guide_model.h / task_guide.cpp. Layout notes in controller/docs/task-guide-ui.md.

#include "fola_eyes.h"
#include "celebration_renderer.h"
#include "fola_gfx.h"
#include "task_guide_model.h"

namespace task_guide {

/* What the screen shows, copied out of the model under the guide lock. */
struct Frame {
    View view;
    bool notice = false; // "暂时没收到结果" after a controller timeout
};

class Screen {
public:
    /* Draws the frame. Only the parts that changed since the previous call
     * are repainted, unless `continuing` is false (another renderer used the
     * canvas in between), which forces a full repaint. */
    void draw(const fola::Canvas &canvas, const Frame &frame, int64_t now_ms, bool continuing);

    /* Builds the eye sprites and track table before the first guide, so the
     * first frames do not stall the LVGL lock. Optional. */
    static void prepare(const fola::Canvas &canvas);
    void release();

    /* Step-number banner at the start of each step. */
    static constexpr uint32_t kBannerMs = 1200;

private:
    enum class Scene : uint8_t { None, Notice, Preview, Banner, Running, Done };
    static Scene scene_for(const Frame &frame);

    void draw_full(const fola::Canvas &c, Scene scene, const Frame &frame, int64_t now_ms);
    void draw_running_time(const fola::Canvas &c, uint32_t remaining_ms, bool repaint);

    Scene scene_ = Scene::None;
    uint32_t boot_ = 0, request_ = 0;
    unsigned step_ = 0;
    int track_level_ = -1;
    unsigned seconds_ = ~0u;
    int64_t scene_started_ms_ = 0;
    fola::EyeMotion eyes_;
    fola::Celebration celebration_;
};

} // namespace task_guide
