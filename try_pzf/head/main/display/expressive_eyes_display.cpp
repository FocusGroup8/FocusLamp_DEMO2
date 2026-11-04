/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "expressive_eyes_display.h"
#include "board_init.h"
#include "pixel_faces_data.h"
#include "pixel_design_renderer.h"
#include "algo_result_manager.h"
#include "focus_ring_renderer.h"
#include "fola_eyes.h"
#include "task_guide.h"
#include "esp_timer.h"
#include "nvs.h"
#include <atomic>
#include <algorithm>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "expressive_eyes.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <cmath>
#include <cstring>
#include <memory>

static const char *TAG = "EXPR_EYES";
static const char *pixel_role_names[] = {"yoyo","diggy","dilly","dash","scout","phew","chacha","tock"};
static std::atomic<int> pixel_role{-1};
static std::atomic<int> pixel_expression{0};
static std::atomic<int> pixel_gaze_x{0}, pixel_gaze_y{0};
static std::atomic<int64_t> pixel_blink_until{0};
static std::atomic<bool> pixel_optimized{false};
static std::atomic<uint32_t> pixel_revision{0};
static std::atomic<int> pixel_visible_expression{0};
static pixel_design::Engine optimized_engine;
static focus_ring::Renderer focus_renderer;
// "fola" role: Figma FOBI glow eyes (fola_eyes.h), the default face.
static std::atomic<bool> fola_role{true};
static fola::EyeMotion fola_motion;

static fola::EyePose fola_pose(int expression)
{
    fola::EyePose p;
    auto both = [&](float open, float lid, float tilt) {
        p.left.open = p.right.open = open;
        p.left.lid = p.right.lid = lid;
        p.left.tilt = p.right.tilt = tilt;
    };
    switch ((expressive_eyes_expression_t)expression) {
    case EXPRESSIVE_EYES_HAPPY: p.left.happy = p.right.happy = true; break;
    case EXPRESSIVE_EYES_SAD: both(1, 0.06f, -0.3f); p.gaze_y = 3; break;
    case EXPRESSIVE_EYES_ANGRY: both(1, 0.08f, 0.38f); break;
    case EXPRESSIVE_EYES_SURPRISED: both(1.12f, 0, 0); break;
    case EXPRESSIVE_EYES_SLEEPY: both(0.22f, 0, 0); p.gaze_y = 3; break; // 休眠眯眯眼
    case EXPRESSIVE_EYES_BORED: both(0.62f, 0.2f, 0); p.gaze_y = 5; break;
    // Labels use the robot's left/right; EyePose uses screen coordinates.
    case EXPRESSIVE_EYES_WINK_LEFT: p.right.open = 0.08f; break;
    case EXPRESSIVE_EYES_WINK_RIGHT: p.left.open = 0.08f; break;
    default: break;
    }
    return p;
}

/* Draws the fola face; repaints the whole canvas only when `full`. */
static void fola_draw(uint8_t *bytes, int width, int height, int stride, int64_t now_ms, bool full)
{
    const fola::Canvas canvas(bytes, width, height, stride);
    const fola::EyeLayout layout = fola::neutral_layout(fola::kTeal);
    if (full) canvas.background(canvas.bounds());
    fola::draw_eyes(canvas, layout, fola_motion.pose(now_ms));
}

static void task_guide_touch_event(lv_event_t *event)
{
    switch(lv_event_get_code(event)) {
    case LV_EVENT_PRESSED: task_guide_touch_pressed(); break;
    case LV_EVENT_RELEASED: task_guide_touch_released(); break;
    case LV_EVENT_PRESS_LOST: task_guide_touch_cancelled(); break;
    default: break;
    }
}

// ============================================================================
// Module state
// ============================================================================

namespace {

// Electric blue eye color (0x00RRGGBB)
static constexpr uint32_t EYE_COLOR_HEX = 0x00BFFF;
static constexpr uint32_t BG_COLOR_HEX  = 0x000000;

struct ExpressiveEyesState {
    std::unique_ptr<espp::ExpressiveEyes> eyes;
    lv_obj_t *canvas                          = nullptr;
    lv_color_t *canvas_buf                    = nullptr; // LVGL color buffer (RGB565)
    int screen_width                          = 0;
    int screen_height                         = 0;
    int original_eye_height                   = 0; // Base eye height (before blink/expression scale)
    int eye_base_width                        = 0; // Base eye width (for eyebrow/cheek sizing)
    TaskHandle_t task_handle                  = nullptr;
    bool running                              = false;
    expressive_eyes_expression_t current_expr = EXPRESSIVE_EYES_NEUTRAL;
};

static ExpressiveEyesState s_state;

// ============================================================================
// LVGL layer API helpers (software rendering in the current configuration)
// ============================================================================

/**
 * @brief Draw a filled ellipse on canvas using LVGL layer API
 */
static void draw_ellipse(lv_obj_t *canvas, int cx, int cy, int width, int height, lv_color_t color)
{
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.bg_color     = color;
    rect_dsc.bg_opa       = LV_OPA_COVER;
    rect_dsc.radius       = LV_RADIUS_CIRCLE;
    rect_dsc.border_width = 0;

    lv_area_t area;
    area.x1 = cx - width / 2;
    area.y1 = cy - height / 2;
    area.x2 = cx + width / 2;
    area.y2 = cy + height / 2;

    lv_draw_rect(&layer, &rect_dsc, &area);
    lv_canvas_finish_layer(canvas, &layer);
}

/**
 * @brief Draw a single eye with eyebrow and cheek using LVGL layer API
 *        Based on the official MonochromeBlueDrawer implementation.
 */
static void draw_single_eye(const espp::ExpressiveEyes::EyeState &eye_state, bool is_left)
{
    // Draw eye ellipse in electric blue
    draw_ellipse(s_state.canvas, eye_state.x, eye_state.y, eye_state.width, eye_state.height,
                 lv_color_hex(EYE_COLOR_HEX));

    // Draw eyebrow as rotated line (black, cuts into the blue eye)
    if (eye_state.expression.eyebrow.enabled) {
        int brow_width  = static_cast<int>(s_state.eye_base_width * eye_state.expression.eyebrow.width * 1.5f);
        int brow_height = static_cast<int>(s_state.eye_base_width * eye_state.expression.eyebrow.thickness * 4.0f);
        int brow_y      = eye_state.y - static_cast<int>(s_state.original_eye_height * 0.4f);

        // Mirror eyebrow angle for right eye
        float angle_rad = eye_state.expression.eyebrow.angle;
        if (!is_left) {
            angle_rad = -angle_rad;
        }

        float half_w = brow_width / 2.0f;
        float cos_a  = cosf(angle_rad);
        float sin_a  = sinf(angle_rad);

        lv_point_precise_t p1, p2;
        p1.x = static_cast<int32_t>(eye_state.x - half_w * cos_a);
        p1.y = static_cast<int32_t>(brow_y - half_w * sin_a);
        p2.x = static_cast<int32_t>(eye_state.x + half_w * cos_a);
        p2.y = static_cast<int32_t>(brow_y + half_w * sin_a);

        lv_layer_t layer;
        lv_canvas_init_layer(s_state.canvas, &layer);

        lv_draw_line_dsc_t line_dsc;
        lv_draw_line_dsc_init(&line_dsc);
        line_dsc.color       = lv_color_hex(BG_COLOR_HEX);
        line_dsc.width       = brow_height;
        line_dsc.opa         = LV_OPA_COVER;
        line_dsc.round_start = 1;
        line_dsc.round_end   = 1;
        line_dsc.p1          = p1;
        line_dsc.p2          = p2;

        lv_draw_line(&layer, &line_dsc);
        lv_canvas_finish_layer(s_state.canvas, &layer);
    }

    // Draw cheek (black, cuts into the blue eye)
    if (eye_state.expression.cheek.enabled) {
        int cheek_width  = static_cast<int>(s_state.eye_base_width * eye_state.expression.cheek.size * 2.5f);
        int cheek_height = static_cast<int>(s_state.eye_base_width * eye_state.expression.cheek.size * 1.5f);
        int cheek_y      = eye_state.y + static_cast<int>(s_state.original_eye_height * 0.4f) +
                           static_cast<int>(eye_state.expression.cheek_offset_y * s_state.screen_height);

        lv_layer_t layer;
        lv_canvas_init_layer(s_state.canvas, &layer);

        lv_draw_rect_dsc_t rect_dsc;
        lv_draw_rect_dsc_init(&rect_dsc);
        rect_dsc.bg_color     = lv_color_hex(BG_COLOR_HEX);
        rect_dsc.bg_opa       = LV_OPA_COVER;
        rect_dsc.radius       = LV_RADIUS_CIRCLE;
        rect_dsc.border_width = 0;

        lv_area_t area;
        area.x1 = eye_state.x - cheek_width / 2;
        area.y1 = cheek_y - cheek_height / 2;
        area.x2 = eye_state.x + cheek_width / 2;
        area.y2 = cheek_y + cheek_height / 2;

        lv_draw_rect(&layer, &rect_dsc, &area);
        lv_canvas_finish_layer(s_state.canvas, &layer);
    }
}

} // anonymous namespace

// ============================================================================
// Expression mapping
// ============================================================================

static espp::ExpressiveEyes::Expression to_cpp_expr(expressive_eyes_expression_t expr)
{
    switch (expr) {
    case EXPRESSIVE_EYES_NEUTRAL:
        return espp::ExpressiveEyes::Expression::NEUTRAL;
    case EXPRESSIVE_EYES_HAPPY:
        return espp::ExpressiveEyes::Expression::HAPPY;
    case EXPRESSIVE_EYES_SAD:
        return espp::ExpressiveEyes::Expression::SAD;
    case EXPRESSIVE_EYES_ANGRY:
        return espp::ExpressiveEyes::Expression::ANGRY;
    case EXPRESSIVE_EYES_SURPRISED:
        return espp::ExpressiveEyes::Expression::SURPRISED;
    case EXPRESSIVE_EYES_SLEEPY:
        return espp::ExpressiveEyes::Expression::SLEEPY;
    case EXPRESSIVE_EYES_BORED:
        return espp::ExpressiveEyes::Expression::BORED;
    case EXPRESSIVE_EYES_WINK_LEFT:
        return espp::ExpressiveEyes::Expression::WINK_LEFT;
    case EXPRESSIVE_EYES_WINK_RIGHT:
        return espp::ExpressiveEyes::Expression::WINK_RIGHT;
    default:
        return espp::ExpressiveEyes::Expression::NEUTRAL;
    }
}

// ============================================================================
// Draw callback (Monochrome Blue style) - uses LVGL layer API
// ============================================================================

static void monochrome_blue_draw(const espp::ExpressiveEyes::EyeState &left_eye,
                                 const espp::ExpressiveEyes::EyeState &right_eye)
{
    if (!s_state.canvas || !s_state.canvas_buf)
        return;

    lvgl_port_lock(0);

    // Clear the shared canvas; no per-role framebuffers are allocated.
    lv_canvas_fill_bg(s_state.canvas, lv_color_hex(BG_COLOR_HEX), LV_OPA_COVER);

    int role = pixel_role.load();
    if (role >= 0) {
        const auto &face = pixel_faces[role][pixel_expression.load()];
        lv_layer_t layer;
        lv_canvas_init_layer(s_state.canvas, &layer);
        int64_t now = esp_timer_get_time();
        // First prototype: neutral idle blink + explicit blink, no speech animation.
        int64_t phase = now % 4800000;
        float open = phase < 280000 ? std::abs(phase - 140000) / 140000.0f : 1.0f;
        int64_t remaining = pixel_blink_until.load() - now;
        if (remaining > 0 && remaining <= 280000)
            open = std::min(open, std::abs(remaining - 140000) / 140000.0f);
        if (pixel_expression.load() != 0 && remaining <= 0) open = 1.0f;
        for (unsigned i = 0; i < face.count; ++i) {
            auto r = face.rects[i];
            float y = r.y, h = r.h;
            int dx = 0, dy = 0;
            if (r.group) {
                y = 246 + (y - 246) * open; h *= open;
                dx = pixel_gaze_x.load(); dy = pixel_gaze_y.load();
            }
            if (h < 1) continue;
            lv_draw_rect_dsc_t d;
            lv_draw_rect_dsc_init(&d);
            d.bg_color = lv_color_hex(r.color); d.bg_opa = LV_OPA_COVER;
            lv_area_t a = {
                (int32_t)((r.x + dx) * s_state.screen_width / 480),
                (int32_t)((y + dy) * s_state.screen_height / 480),
                (int32_t)((r.x + r.w + dx) * s_state.screen_width / 480 - 1),
                (int32_t)((y + h + dy) * s_state.screen_height / 480 - 1)};
            lv_draw_rect(&layer, &d, &a);
        }
        lv_canvas_finish_layer(s_state.canvas, &layer);
        lvgl_port_unlock();
        return;
    }
    // Draw both eyes
    draw_single_eye(left_eye, true);
    draw_single_eye(right_eye, false);

    lvgl_port_unlock();
}

// ============================================================================
// Animation task
// ============================================================================

static void expressive_eyes_task(void *arg)
{
    ESP_LOGI(TAG, "Animation task started");

    auto last_time  = xTaskGetTickCount();
    s_state.running = true;

    int64_t report_at=esp_timer_get_time();
    uint64_t draw_sum=0;uint32_t draw_max=0,draw_frames=0;
    int64_t last_blink_request=0;
    // Which renderer drew the previous frame; the fola face repaints fully after another one.
    enum class Drawer { None, Guide, Focus, Pixel, Legacy, Fola } last_drawer = Drawer::None;
    uint32_t fola_revision = pixel_revision.load();
    int fola_gaze_x = 0, fola_gaze_y = 0;
    while (s_state.running) {
        TickType_t now = xTaskGetTickCount();
        float dt       = (float)(now - last_time) * portTICK_PERIOD_MS / 1000.0f;
        last_time      = now;

        focus_view_t focus; focus_sync_get_view(&focus);
        const bool optimized = pixel_role.load() >= 0 && pixel_optimized.load();
        const bool fola_face = fola_role.load();
        bool guide_drawn=false;
        if(s_state.canvas && s_state.canvas_buf) {
            lvgl_port_lock(0);
            auto *buffer=lv_canvas_get_draw_buf(s_state.canvas);
            guide_drawn=task_guide_draw(reinterpret_cast<uint8_t*>(s_state.canvas_buf),
                s_state.screen_width,s_state.screen_height,(int)buffer->header.stride,esp_timer_get_time()/1000);
            if(guide_drawn)lv_obj_invalidate(s_state.canvas);
            lvgl_port_unlock();
        }
        if(guide_drawn) {
            // Timer and screen releases stay local even when discovery/HTTP is unavailable.
            last_drawer = Drawer::Guide;
        } else if (focus.focus && s_state.canvas && s_state.canvas_buf) {
            last_drawer = Drawer::Focus;
            const int64_t begin=esp_timer_get_time();
            lvgl_port_lock(0);
            auto *buffer=lv_canvas_get_draw_buf(s_state.canvas);
            focus_renderer.draw({reinterpret_cast<uint8_t*>(s_state.canvas_buf),s_state.screen_width,
                s_state.screen_height,(int)buffer->header.stride},focus,begin/1000);
            lv_obj_invalidate(s_state.canvas);
            lvgl_port_unlock();
            uint32_t us=(uint32_t)(esp_timer_get_time()-begin);draw_sum+=us;draw_max=std::max(draw_max,us);draw_frames++;
        } else if (fola_face && s_state.canvas && s_state.canvas_buf) {
            const int64_t begin=esp_timer_get_time();
            const int64_t now_ms=begin/1000;
            const bool full = last_drawer != Drawer::Fola;
            if (full) {
                fola_motion.reset(fola_pose(pixel_expression.load()), now_ms);
                fola_revision = pixel_revision.load();
            } else if (pixel_revision.load() != fola_revision) {
                fola_revision = pixel_revision.load();
                fola_motion.set(fola_pose(pixel_expression.load()), now_ms);
            }
            const int64_t blink=pixel_blink_until.load();
            if(blink!=last_blink_request){last_blink_request=blink;if(blink)fola_motion.blink(now_ms);}
            if (pixel_gaze_x.load() != fola_gaze_x || pixel_gaze_y.load() != fola_gaze_y) {
                fola_gaze_x = pixel_gaze_x.load(); fola_gaze_y = pixel_gaze_y.load();
                if (fola_gaze_x || fola_gaze_y) fola_motion.look(fola_gaze_x / 12.0f, fola_gaze_y / 6.0f);
                else fola_motion.clear_look();
            }
            lvgl_port_lock(0);
            auto *buffer=lv_canvas_get_draw_buf(s_state.canvas);
            fola_draw(reinterpret_cast<uint8_t*>(s_state.canvas_buf),s_state.screen_width,s_state.screen_height,
                (int)buffer->header.stride,now_ms,full);
            lv_obj_invalidate(s_state.canvas);
            lvgl_port_unlock();
            last_drawer = Drawer::Fola;
            uint32_t us=(uint32_t)(esp_timer_get_time()-begin);draw_sum+=us;draw_max=std::max(draw_max,us);draw_frames++;
        } else if (optimized && s_state.canvas && s_state.canvas_buf) {
            last_drawer = Drawer::Pixel;
            const int64_t begin=esp_timer_get_time();
            const int role=pixel_role.load();
            // A concurrent role change is rechecked before indexing the profile table.
            if (role >= 0 && role < 8) {
                optimized_engine.update(role,pixel_expression.load(),pixel_revision.load(),begin/1000,algo_result_expression_held());
                const int64_t blink=pixel_blink_until.load();
                if(blink!=last_blink_request){last_blink_request=blink;if(blink)optimized_engine.blinkAt=(blink-280000)/1000;}
                lvgl_port_lock(0);
                auto *buffer=lv_canvas_get_draw_buf(s_state.canvas);
                pixel_design::render({reinterpret_cast<uint8_t*>(s_state.canvas_buf),s_state.screen_width,
                    s_state.screen_height,(int)buffer->header.stride},optimized_engine,begin/1000,
                    pixel_gaze_x.load(),pixel_gaze_y.load());
                lv_obj_invalidate(s_state.canvas);
                lvgl_port_unlock();
                pixel_visible_expression.store(optimized_engine.visible);
                uint32_t us=(uint32_t)(esp_timer_get_time()-begin);draw_sum+=us;draw_max=std::max(draw_max,us);draw_frames++;
            }
        } else if (s_state.eyes) {
            last_drawer = Drawer::Legacy;
            optimized_engine.role=-1;
            s_state.eyes->update(dt);
        }
        int64_t now_us=esp_timer_get_time();
        if(now_us-report_at>=5000000){
            if(draw_frames)ESP_LOGI(TAG,"%s frames=%lu avg_us=%lu max_us=%lu internal_free=%u internal_min=%u psram_free=%u psram_largest=%u engine_bytes=%u",
                focus.focus ? "[FOCUS_PERF]" : (fola_face ? "[FOLA_PERF]" : "[PIXEL_PERF]"), (unsigned long)draw_frames,(unsigned long)(draw_sum/draw_frames),(unsigned long)draw_max,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),(unsigned)(focus.focus?sizeof(focus_renderer):sizeof(optimized_engine)));
            report_at=now_us;draw_sum=0;draw_max=draw_frames=0;
        }
        if(guide_drawn || optimized || focus.focus || fola_face) {
            const TickType_t period = pdMS_TO_TICKS(33);
            // A slow LVGL flush can take longer than the whole frame budget.
            // Do not replay missed deadlines in a tight catch-up loop: both
            // the producer and LVGL need idle time, even while overloaded.
            if ((TickType_t)(xTaskGetTickCount() - last_time) >= period) {
                vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS(10)));
                last_time = xTaskGetTickCount();
            } else {
                vTaskDelayUntil(&last_time, period);
            }
        } else vTaskDelay(pdMS_TO_TICKS(16));
    }

    ESP_LOGI(TAG, "Animation task stopped");
    s_state.task_handle = nullptr;
    vTaskDelete(NULL);
}

// ============================================================================
// Public C API implementation
// ============================================================================

esp_err_t expressive_eyes_display_init(const expressive_eyes_cfg_t *cfg)
{
    if (!cfg || !cfg->disp) {
        ESP_LOGE(TAG, "Invalid configuration");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_state.eyes) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing expressive eyes display (%dx%d)...", cfg->screen_width, cfg->screen_height);

    s_state.screen_width  = cfg->screen_width;
    s_state.screen_height = cfg->screen_height;

    // Calculate base eye dimensions (same as official example)
    s_state.eye_base_width      = (int)(cfg->screen_width * 0.35f);
    s_state.original_eye_height = (int)(cfg->screen_height * 0.55f);

    // Step 1: Allocate canvas buffer from PSRAM
    size_t buf_size    = (size_t)cfg->screen_width * cfg->screen_height * sizeof(lv_color_t);
    s_state.canvas_buf = (lv_color_t *)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_state.canvas_buf) {
        ESP_LOGE(TAG, "Failed to allocate canvas buffer (%zu bytes)", buf_size);
        return ESP_ERR_NO_MEM;
    }
    memset(s_state.canvas_buf, 0, buf_size);
    ESP_LOGI(TAG, "Canvas buffer allocated (PSRAM, %zu bytes)", buf_size);

    // Step 2: Create LVGL canvas
    lvgl_port_lock(0);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(BG_COLOR_HEX), 0);

    s_state.canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_state.canvas, s_state.canvas_buf, cfg->screen_width, cfg->screen_height,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_center(s_state.canvas);
    lv_obj_set_clickable(s_state.canvas, true);
    lv_obj_set_scrollable(s_state.canvas, false);
    lv_obj_add_event_cb(s_state.canvas, task_guide_touch_event, LV_EVENT_ALL, nullptr);
    task_guide_set_touch_available(cfg->touch_available);

    lvgl_port_unlock();
    ESP_LOGI(TAG, "Canvas created");

    // Step 3: Configure ExpressiveEyes with adaptive sizing for 480x480
    int large_eye_width  = s_state.eye_base_width;
    int large_eye_height = s_state.original_eye_height;
    int large_spacing    = (int)(cfg->screen_width * 0.55f);

    espp::ExpressiveEyes::Config eyes_cfg;
    eyes_cfg.screen_width         = cfg->screen_width;
    eyes_cfg.screen_height        = cfg->screen_height;
    eyes_cfg.eye_spacing          = large_spacing;
    eyes_cfg.eye_width            = large_eye_width;
    eyes_cfg.eye_height           = large_eye_height;
    eyes_cfg.blink_duration       = cfg->blink_duration > 0 ? cfg->blink_duration : 0.12f;
    eyes_cfg.blink_interval       = cfg->blink_interval > 0 ? cfg->blink_interval : 4.0f;
    eyes_cfg.enable_auto_blink    = cfg->enable_auto_blink;
    eyes_cfg.enable_pupil_physics = cfg->enable_pupil_physics;
    eyes_cfg.on_draw              = monochrome_blue_draw;
    eyes_cfg.log_level            = espp::Logger::Verbosity::WARN;

    s_state.eyes = std::make_unique<espp::ExpressiveEyes>(eyes_cfg);
    if (!s_state.eyes) {
        ESP_LOGE(TAG, "Failed to create ExpressiveEyes object");
        heap_caps_free(s_state.canvas_buf);
        s_state.canvas_buf = nullptr;
        lvgl_port_lock(0);
        lv_obj_clean(scr);
        lvgl_port_unlock();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "ExpressiveEyes created (eye %dx%d, spacing %d)", large_eye_width, large_eye_height, large_spacing);

    // Restore the last successfully stored selection before the first frame.
    nvs_handle_t saved;
    esp_err_t boot_result=nvs_open("face_boot",NVS_READONLY,&saved);
    if(boot_result==ESP_OK){
        char selection[40]={0};size_t length=sizeof(selection);
        boot_result=nvs_get_str(saved,"selection",selection,&length);
        if(boot_result==ESP_OK){
            char* separator=strchr(selection,':');
            if(separator){*separator='\0';
                if(!strcmp(selection,"original")||!strcmp(selection,"optimized")){
                    if(expressive_eyes_set_role(separator+1)==ESP_OK){
                        expressive_eyes_set_style(selection);
                        ESP_LOGI(TAG,"[FACE_BOOT] Restored role=%s style=%s before animation task",separator+1,selection);
                    }else ESP_LOGW(TAG,"[FACE_BOOT] Stored role is unsupported");
                }
                else ESP_LOGW(TAG,"[FACE_BOOT] Stored style is unsupported");
            }else ESP_LOGW(TAG,"[FACE_BOOT] Stored selection has no separator");
        }
        else ESP_LOGW(TAG,"[FACE_BOOT] Read selection: %s",esp_err_to_name(boot_result));
        nvs_close(saved);
    }else ESP_LOGW(TAG,"[FACE_BOOT] Open selection: %s; using default fola eyes",esp_err_to_name(boot_result));
    // Eye sprites and the task track table take a moment to build; do it
    // now, while the backlight is off, instead of on the first guide frame.
    {
        lvgl_port_lock(0);
        auto *buffer = lv_canvas_get_draw_buf(s_state.canvas);
        task_guide_prepare(reinterpret_cast<uint8_t *>(s_state.canvas_buf), s_state.screen_width,
            s_state.screen_height, (int)buffer->header.stride);
        lvgl_port_unlock();
    }
    // Prepare the restored face while the backlight is still off. Never expose
    // an intermediate default frame while waiting for the animation task.
    if (fola_role.load()) {
        const int64_t now = esp_timer_get_time() / 1000;
        fola_motion.reset(fola_pose(pixel_expression.load()), now);
        lvgl_port_lock(0);
        auto *buffer = lv_canvas_get_draw_buf(s_state.canvas);
        fola_draw(reinterpret_cast<uint8_t *>(s_state.canvas_buf), s_state.screen_width, s_state.screen_height,
            (int)buffer->header.stride, now, true);
        lv_obj_invalidate(s_state.canvas);
        lvgl_port_unlock();
    } else if (pixel_optimized.load() && pixel_role.load() >= 0) {
        const int64_t now = esp_timer_get_time() / 1000;
        optimized_engine.update(pixel_role.load(), pixel_expression.load(), pixel_revision.load(), now,algo_result_expression_held());
        lvgl_port_lock(0);
        auto *buffer = lv_canvas_get_draw_buf(s_state.canvas);
        pixel_design::render({reinterpret_cast<uint8_t *>(s_state.canvas_buf), s_state.screen_width,
            s_state.screen_height, (int)buffer->header.stride}, optimized_engine, now, 0, 0);
        lv_obj_invalidate(s_state.canvas);
        lvgl_port_unlock();
    } else {
        s_state.eyes->update(0.034f);
    }
    lvgl_port_lock(0);
    lv_refr_now(cfg->disp);
    lvgl_port_unlock();
    // Two 60 Hz scan periods let the newly flushed picture reach the panel.
    vTaskDelay(pdMS_TO_TICKS(40));
    board_lcd_set_backlight(true);
    ESP_LOGI(TAG, "[FACE_BOOT] First face submitted; backlight enabled");

    // Step 4: Start animation task
    BaseType_t ret = xTaskCreatePinnedToCore(expressive_eyes_task, "expr_eyes",
                                             6 * 1024, // stack size
                                             nullptr,
                                             3, // priority (below LVGL task at 4)
                                             &s_state.task_handle,
                                             1 // pin to core 1
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create animation task");
        s_state.eyes.reset();
        heap_caps_free(s_state.canvas_buf);
        s_state.canvas_buf = nullptr;
        lvgl_port_lock(0);
        lv_obj_clean(scr);
        lvgl_port_unlock();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Expressive eyes display initialized");
    return ESP_OK;
}

esp_err_t expressive_eyes_display_deinit(void)
{
    ESP_LOGI(TAG, "Deinitializing expressive eyes display...");
    task_guide_set_touch_available(false);

    // Stop animation task
    s_state.running = false;
    int timeout_ms  = 500;
    while (s_state.task_handle && timeout_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
        timeout_ms -= 10;
    }

    // Release C++ object
    s_state.eyes.reset();

    // Free canvas buffer
    if (s_state.canvas_buf) {
        heap_caps_free(s_state.canvas_buf);
        s_state.canvas_buf = nullptr;
    }

    // Clear screen
    if (s_state.canvas) {
        lvgl_port_lock(0);
        lv_obj_t *scr = lv_screen_active();
        lv_obj_clean(scr);
        lv_obj_set_style_bg_color(scr, lv_color_hex(BG_COLOR_HEX), 0);
        lvgl_port_unlock();
    }

    s_state.canvas       = nullptr;
    s_state.current_expr = EXPRESSIVE_EYES_NEUTRAL;

    ESP_LOGI(TAG, "Expressive eyes display deinitialized");
    return ESP_OK;
}

void expressive_eyes_set_expression(expressive_eyes_expression_t expr)
{
    if ((unsigned)expr > EXPRESSIVE_EYES_WINK_RIGHT) return;
    pixel_expression.store((int)expr);
    pixel_revision.fetch_add(1);
    if (s_state.eyes) {
        s_state.eyes->set_expression(to_cpp_expr(expr));
        s_state.current_expr = expr;
    }
}

void expressive_eyes_look_at(float x, float y)
{
    if (!std::isfinite(x) || !std::isfinite(y)) return;
    pixel_gaze_x.store((int)(std::max(-1.0f, std::min(1.0f, x)) * 12));
    pixel_gaze_y.store((int)(std::max(-1.0f, std::min(1.0f, y)) * 6));
    if (s_state.eyes) {
        s_state.eyes->look_at(x, y);
    }
}

void expressive_eyes_blink(void)
{
    pixel_blink_until.store(esp_timer_get_time() + 280000);
    if (s_state.eyes) {
        s_state.eyes->blink();
    }
}

expressive_eyes_expression_t expressive_eyes_get_expression(void)
{
    if(pixel_optimized.load()&&pixel_role.load()>=0)return (expressive_eyes_expression_t)pixel_visible_expression.load();
    return s_state.current_expr;
}

esp_err_t expressive_eyes_set_role(const char *role)
{
    if (!role) return ESP_ERR_INVALID_ARG;
    if (!strcmp(role, "fola")) {
        pixel_gaze_x.store(0); pixel_gaze_y.store(0); pixel_blink_until.store(0);
        pixel_role.store(-1);
        fola_role.store(true);
        pixel_revision.fetch_add(1);
        return ESP_OK;
    }
    int selected = -2;
    if (!strcmp(role, "legacy")) selected = -1;
    for (int i = 0; i < 8; ++i) if (!strcmp(role, pixel_role_names[i])) selected = i;
    if (selected == -2) return ESP_ERR_INVALID_ARG;
    pixel_gaze_x.store(0); pixel_gaze_y.store(0); pixel_blink_until.store(0);
    fola_role.store(false);
    pixel_role.store(selected);
    pixel_revision.fetch_add(1);
    return ESP_OK;
}

const char *expressive_eyes_get_role(void)
{
    if (fola_role.load()) return "fola";
    int role = pixel_role.load();
    return role < 0 ? "legacy" : pixel_role_names[role];
}

esp_err_t expressive_eyes_set_style(const char *style)
{
    if(!style || (strcmp(style,"original")&&strcmp(style,"optimized")))return ESP_ERR_INVALID_ARG;
    pixel_optimized.store(!strcmp(style,"optimized"));pixel_revision.fetch_add(1);return ESP_OK;
}
const char *expressive_eyes_get_style(void){return pixel_optimized.load()?"optimized":"original";}

esp_err_t expressive_eyes_save_selection(void){
    char selection[40];snprintf(selection,sizeof(selection),"%s:%s",expressive_eyes_get_style(),expressive_eyes_get_role());
    nvs_handle_t handle;esp_err_t result=nvs_open("face_boot",NVS_READWRITE,&handle);if(result!=ESP_OK)return result;
    char previous[40]={0};size_t length=sizeof(previous);
    if(nvs_get_str(handle,"selection",previous,&length)==ESP_OK&&!strcmp(previous,selection)){nvs_close(handle);ESP_LOGI(TAG,"[FACE_BOOT] Saved selection already matches %s",selection);return ESP_OK;}
    result=nvs_set_str(handle,"selection",selection);if(result==ESP_OK)result=nvs_commit(handle);nvs_close(handle);
    if(result==ESP_OK)ESP_LOGI(TAG,"[FACE_BOOT] Selection committed: %s",selection);
    else ESP_LOGE(TAG,"[FACE_BOOT] Selection commit failed: %s",esp_err_to_name(result));
    return result;
}
