from pathlib import Path
p=Path('head/main/display/expressive_eyes_display.cpp');s=p.read_text(encoding='utf8').replace('#include "pixel_design_renderer.h"','#include "pixel_design_renderer.h"\n#include "focus_ring_renderer.h"');s=s.replace('static pixel_design::Engine optimized_engine;','static pixel_design::Engine optimized_engine;\nstatic focus_ring::Renderer focus_renderer;');s=s.replace('        const bool optimized = pixel_role.load()', '        focus_view_t focus; focus_sync_get_view(&focus);\n        const bool optimized = pixel_role.load()');s=s.replace('        if (optimized && s_state.canvas && s_state.canvas_buf) {','''        if (focus.focus && s_state.canvas && s_state.canvas_buf) {
            const int64_t begin=esp_timer_get_time();
            lvgl_port_lock(0);
            auto *buffer=lv_canvas_get_draw_buf(s_state.canvas);
            focus_renderer.draw({reinterpret_cast<uint8_t*>(s_state.canvas_buf),s_state.screen_width,
                s_state.screen_height,(int)buffer->header.stride},focus,begin/1000);
            lv_obj_invalidate(s_state.canvas);
            lvgl_port_unlock();
            uint32_t us=(uint32_t)(esp_timer_get_time()-begin);draw_sum+=us;draw_max=std::max(draw_max,us);draw_frames++;
        } else if (optimized && s_state.canvas && s_state.canvas_buf) {''');s=s.replace('        if(optimized) vTaskDelayUntil','        if(optimized || focus.focus) vTaskDelayUntil');p.write_text(s,encoding='utf8')
