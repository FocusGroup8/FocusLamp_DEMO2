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
#include "fola_face.h"
#include "task_guide.h"
#include "esp_timer.h"
#include "nvs.h"
#include <atomic>
#include <algorithm>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <cmath>
#include <cstring>

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
static fola::FaceRenderer default_face;
static std::atomic<int> fola_label{0};
static_assert(static_cast<int>(fola::FaceLabel::Neutral)==EXPRESSIVE_EYES_NEUTRAL);
static_assert(static_cast<int>(fola::FaceLabel::WinkRight)==EXPRESSIVE_EYES_WINK_RIGHT);
static_assert(static_cast<int>(fola::FaceLabel::Thinking)==9);
static_assert(static_cast<int>(fola::FaceLabel::Speaking)==12);
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

static constexpr uint32_t BG_COLOR_HEX  = 0x000000;

struct ExpressiveEyesState {
    lv_obj_t *canvas                          = nullptr;
    lv_color_t *canvas_buf                    = nullptr; // LVGL color buffer (RGB565)
    int screen_width                          = 0;
    int screen_height                         = 0;
    TaskHandle_t task_handle                  = nullptr;
    std::atomic<bool> running{false};
};

static ExpressiveEyesState s_state;

} // anonymous namespace

// The original eight pixel roles use their own geometry and animation state.
// They do not require the removed blue-eye/espp renderer.
static void draw_pixel_original()
{
    if (!s_state.canvas || !s_state.canvas_buf)
        return;

    lvgl_port_lock(0);

    // Clear the shared canvas; no per-role framebuffers are allocated.
    lv_canvas_fill_bg(s_state.canvas, lv_color_hex(BG_COLOR_HEX), LV_OPA_COVER);

    int role = pixel_role.load();
    if (role >= 0 && role < 8) {
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
    lvgl_port_unlock();
}

// Only the animation task owns the default-face renderer and its cache.
static void draw_default_face(int64_t now_ms, bool entering)
{
    if (!s_state.canvas || !s_state.canvas_buf) return;
    static int64_t last_blink = 0;
    if (entering) default_face.reset(now_ms);
    default_face.set_label(static_cast<fola::FaceLabel>(fola_label.load()), now_ms);
    default_face.look_at(pixel_gaze_x.load() / 12.0f, pixel_gaze_y.load() / 6.0f);
    const int64_t request = pixel_blink_until.load();
    if (request != last_blink) {
        last_blink = request;
        if (request > now_ms * 1000) default_face.blink(now_ms);
    }
    lvgl_port_lock(0);
    auto *buffer = lv_canvas_get_draw_buf(s_state.canvas);
    fola::Canvas canvas(reinterpret_cast<uint8_t *>(s_state.canvas_buf),
        s_state.screen_width, s_state.screen_height, (int)buffer->header.stride);
    // Prepare only the selected expression, before its first visible frame.
    if (entering) default_face.prepare(canvas);
    default_face.draw(canvas, now_ms);
    lv_obj_invalidate(s_state.canvas);
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
    bool focus_drawn_last=false;
    bool default_drawn_last=false;
    while (s_state.running) {
        TickType_t now = xTaskGetTickCount();
        last_time      = now;

        focus_view_t focus; focus_sync_get_view(&focus);
        const bool optimized = pixel_role.load() >= 0 && pixel_optimized.load();
        bool guide_drawn=false;
        if(s_state.canvas && s_state.canvas_buf) {
            lvgl_port_lock(0);
            auto *buffer=lv_canvas_get_draw_buf(s_state.canvas);
            guide_drawn=task_guide_draw(reinterpret_cast<uint8_t*>(s_state.canvas_buf),
                s_state.screen_width,s_state.screen_height,(int)buffer->header.stride,esp_timer_get_time()/1000);
            if(guide_drawn)lv_obj_invalidate(s_state.canvas);
            lvgl_port_unlock();
        }
        const bool draw_focus=!guide_drawn && focus.focus && s_state.canvas && s_state.canvas_buf;
        const bool draw_default=!guide_drawn && !draw_focus && pixel_role.load()<0;
        if (default_drawn_last && !draw_default) default_face.release();
        if(focus_drawn_last && !draw_focus) {
            focus_renderer.release();
            // Guide and focus share sprites. Do not free the guide's freshly
            // drawn eyes when it has taken ownership of this frame.
            if(!guide_drawn)fola::release_eye_cache();
        }
        if(guide_drawn) {
            // Timer and screen releases stay local even when discovery/HTTP is unavailable.
        } else if (draw_focus) {
            const int64_t begin=esp_timer_get_time();
            lvgl_port_lock(0);
            auto *buffer=lv_canvas_get_draw_buf(s_state.canvas);
            focus_renderer.draw({reinterpret_cast<uint8_t*>(s_state.canvas_buf),s_state.screen_width,
                s_state.screen_height,(int)buffer->header.stride},focus,begin/1000,focus_drawn_last);
            lv_obj_invalidate(s_state.canvas);
            lvgl_port_unlock();
            uint32_t us=(uint32_t)(esp_timer_get_time()-begin);draw_sum+=us;draw_max=std::max(draw_max,us);draw_frames++;
        } else if (optimized && s_state.canvas && s_state.canvas_buf) {
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
        } else {
            optimized_engine.role=-1;
            const int64_t begin=esp_timer_get_time();
            if (draw_default) draw_default_face(begin/1000, !default_drawn_last);
            else draw_pixel_original();
            uint32_t us=(uint32_t)(esp_timer_get_time()-begin);draw_sum+=us;draw_max=std::max(draw_max,us);draw_frames++;
        }
        default_drawn_last=draw_default;
        focus_drawn_last=draw_focus;
        int64_t now_us=esp_timer_get_time();
        if(now_us-report_at>=5000000){
            if(draw_frames)ESP_LOGI(TAG,"%s frames=%lu avg_us=%lu max_us=%lu internal_free=%u internal_min=%u psram_free=%u psram_largest=%u engine_bytes=%u face_cache_bytes=%u",
                focus.focus ? "[FOCUS_PERF]" : pixel_role.load()<0 ? "[FOLA_PERF]" : "[PIXEL_PERF]", (unsigned long)draw_frames,(unsigned long)(draw_sum/draw_frames),(unsigned long)draw_max,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                (unsigned)(focus.focus?sizeof(focus_renderer):pixel_role.load()<0?sizeof(default_face):sizeof(optimized_engine)),
                (unsigned)default_face.cache_bytes());
            report_at=now_us;draw_sum=0;draw_max=draw_frames=0;
        }
        {
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
        }
    }

    default_face.release();
    task_guide_release_graphics();
    focus_renderer.release();
    fola::release_eye_cache();
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

    if (s_state.canvas) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing expressive eyes display (%dx%d)...", cfg->screen_width, cfg->screen_height);

    s_state.screen_width  = cfg->screen_width;
    s_state.screen_height = cfg->screen_height;

    pixel_role.store(-1); pixel_expression.store(0); fola_label.store(0);
    pixel_gaze_x.store(0); pixel_gaze_y.store(0); pixel_blink_until.store(0);
    default_face.reset(esp_timer_get_time()/1000);
    default_face.configure_auto_blink(cfg->enable_auto_blink,
        cfg->blink_interval>0 ? cfg->blink_interval : 4.0f,
        cfg->blink_duration>0 ? cfg->blink_duration : 0.12f);

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

    // Restore the last successfully stored selection before the first frame.
    bool migrate_selection=false;
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
                        migrate_selection=!strcmp(separator+1,"legacy");
                        ESP_LOGI(TAG,"[FACE_BOOT] Restored role=%s style=%s before animation task",expressive_eyes_get_role(),expressive_eyes_get_style());
                    }else ESP_LOGW(TAG,"[FACE_BOOT] Stored role is unsupported");
                }
                else ESP_LOGW(TAG,"[FACE_BOOT] Stored style is unsupported");
            }else ESP_LOGW(TAG,"[FACE_BOOT] Stored selection has no separator");
        }
        else ESP_LOGW(TAG,"[FACE_BOOT] Read selection: %s",esp_err_to_name(boot_result));
        nvs_close(saved);
    }else ESP_LOGW(TAG,"[FACE_BOOT] Open selection: %s; using default FOLA eyes",esp_err_to_name(boot_result));
    if(migrate_selection && expressive_eyes_save_selection()!=ESP_OK)
        ESP_LOGW(TAG,"[FACE_BOOT] FOLA selected; old selection could not be persisted");
    // Prepare the restored face while the backlight is still off. Never expose
    // an intermediate default frame while waiting for the animation task.
    if (pixel_optimized.load() && pixel_role.load() >= 0) {
        const int64_t now = esp_timer_get_time() / 1000;
        optimized_engine.update(pixel_role.load(), pixel_expression.load(), pixel_revision.load(), now,algo_result_expression_held());
        lvgl_port_lock(0);
        auto *buffer = lv_canvas_get_draw_buf(s_state.canvas);
        pixel_design::render({reinterpret_cast<uint8_t *>(s_state.canvas_buf), s_state.screen_width,
            s_state.screen_height, (int)buffer->header.stride}, optimized_engine, now, 0, 0);
        lv_obj_invalidate(s_state.canvas);
        lvgl_port_unlock();
    } else if (pixel_role.load()<0) {
        draw_default_face(esp_timer_get_time()/1000, true);
    } else {
        draw_pixel_original();
    }
    lvgl_port_lock(0);
    lv_refr_now(cfg->disp);
    lvgl_port_unlock();
    // Two 60 Hz scan periods let the newly flushed picture reach the panel.
    vTaskDelay(pdMS_TO_TICKS(40));
    board_lcd_set_backlight(true);
    ESP_LOGI(TAG, "[FACE_BOOT] First face submitted; backlight enabled");

    // Step 4: Start animation task
    s_state.running=true;
    BaseType_t ret = xTaskCreatePinnedToCore(expressive_eyes_task, "expr_eyes",
                                             6 * 1024, // stack size
                                             nullptr,
                                             3, // priority (below LVGL task at 4)
                                             &s_state.task_handle,
                                             1 // pin to core 1
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create animation task");
        s_state.running=false;
        default_face.release();
        heap_caps_free(s_state.canvas_buf);
        s_state.canvas_buf = nullptr;
        lvgl_port_lock(0);
        lv_obj_clean(scr);
        s_state.canvas=nullptr;
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
    if (s_state.task_handle) {
        ESP_LOGE(TAG, "Animation task is still releasing its renderer; canvas retained");
        return ESP_ERR_TIMEOUT;
    }

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
    pixel_expression.store(EXPRESSIVE_EYES_NEUTRAL);
    fola_label.store(0);

    ESP_LOGI(TAG, "Expressive eyes display deinitialized");
    return ESP_OK;
}

void expressive_eyes_set_expression(expressive_eyes_expression_t expr)
{
    if ((unsigned)expr > EXPRESSIVE_EYES_WINK_RIGHT) return;
    pixel_expression.store((int)expr);
    pixel_revision.fetch_add(1);
    fola_label.store((int)expr);
}

void expressive_eyes_look_at(float x, float y)
{
    if (!std::isfinite(x) || !std::isfinite(y)) return;
    pixel_gaze_x.store((int)(std::max(-1.0f, std::min(1.0f, x)) * 12));
    pixel_gaze_y.store((int)(std::max(-1.0f, std::min(1.0f, y)) * 6));
}

void expressive_eyes_blink(void)
{
    pixel_blink_until.store(esp_timer_get_time() + 280000);
}

expressive_eyes_expression_t expressive_eyes_get_expression(void)
{
    if(pixel_optimized.load()&&pixel_role.load()>=0)return (expressive_eyes_expression_t)pixel_visible_expression.load();
    return (expressive_eyes_expression_t)pixel_expression.load();
}

esp_err_t expressive_eyes_set_role(const char *role)
{
    if (!role) return ESP_ERR_INVALID_ARG;
    int selected = -2;
    // Old saved selections and clients keep working; neither alias draws blue eyes.
    if (!strcmp(role, "legacy") || !strcmp(role, "fola")) selected = -1;
    for (int i = 0; i < 8; ++i) if (!strcmp(role, pixel_role_names[i])) selected = i;
    if (selected == -2) return ESP_ERR_INVALID_ARG;
    pixel_gaze_x.store(0); pixel_gaze_y.store(0); pixel_blink_until.store(0);
    pixel_role.store(selected);
    pixel_revision.fetch_add(1);
    return ESP_OK;
}

const char *expressive_eyes_get_role(void)
{
    int role = pixel_role.load();
    return role < 0 ? "fola" : pixel_role_names[role];
}

esp_err_t expressive_eyes_set_style(const char *style)
{
    if(!style || (strcmp(style,"original")&&strcmp(style,"optimized")))return ESP_ERR_INVALID_ARG;
    pixel_optimized.store(!strcmp(style,"optimized"));pixel_revision.fetch_add(1);return ESP_OK;
}
const char *expressive_eyes_get_style(void){return pixel_role.load() >= 0 && pixel_optimized.load() ? "optimized" : "original";}

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

esp_err_t expressive_eyes_set_fola_label(const char *name)
{
    fola::FaceLabel label;
    if (!fola::face_label_from_name(name,label)) return ESP_ERR_INVALID_ARG;
    // Keep the nine-value public expression contract and pixel-role tables intact.
    const int id=static_cast<int>(label);
    static const int fallback[]={EXPRESSIVE_EYES_BORED,EXPRESSIVE_EYES_SURPRISED,
        EXPRESSIVE_EYES_NEUTRAL,EXPRESSIVE_EYES_HAPPY};
    expressive_eyes_set_expression((expressive_eyes_expression_t)(id<9 ? id : fallback[id-9]));
    fola_label.store(id);
    return ESP_OK;
}

const char *expressive_eyes_get_fola_label(void)
{
    const int label=pixel_role.load()<0 ? fola_label.load() : (int)expressive_eyes_get_expression();
    return fola::face_label_name(static_cast<fola::FaceLabel>(label));
}
