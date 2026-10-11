/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "expressive_eyes_display.h"

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
// LVGL layer API helpers (hardware-accelerated via ESP32-P4 PPA)
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

    // Clear canvas with black background (GPU-accelerated fill)
    lv_canvas_fill_bg(s_state.canvas, lv_color_hex(BG_COLOR_HEX), LV_OPA_COVER);

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

    while (s_state.running) {
        TickType_t now = xTaskGetTickCount();
        float dt       = (float)(now - last_time) * portTICK_PERIOD_MS / 1000.0f;
        last_time      = now;

        if (s_state.eyes) {
            s_state.eyes->update(dt);
        }

        vTaskDelay(pdMS_TO_TICKS(16)); // ~60 FPS
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
    if (s_state.eyes) {
        s_state.eyes->set_expression(to_cpp_expr(expr));
        s_state.current_expr = expr;
    }
}

void expressive_eyes_look_at(float x, float y)
{
    if (s_state.eyes) {
        s_state.eyes->look_at(x, y);
    }
}

void expressive_eyes_blink(void)
{
    if (s_state.eyes) {
        s_state.eyes->blink();
    }
}

expressive_eyes_expression_t expressive_eyes_get_expression(void)
{
    return s_state.current_expr;
}
