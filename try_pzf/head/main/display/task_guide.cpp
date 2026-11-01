#include "task_guide.h"
#include "task_guide_model.h"
#include "cjk_font_16.h"
#include "pixel_design_renderer.h"
#include "timer_ring_renderer.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {
portMUX_TYPE guard = portMUX_INITIALIZER_UNLOCKED;
task_guide::Model model;
bool touch_available=false, pressed=false;
uint32_t pressed_boot=0,pressed_request=0;
uint32_t report_sequence=0,acked_sequence=0,model_revision=0;
int64_t last_report_ms=0,notice_until_ms=0;
uint32_t waiting_boot=0,waiting_request=0;

// The normal, non-focus screen is mode=idle. Companion is an optional named mode,
// not a prerequisite for a user asking Xiaozhi to break a task into three steps.
bool guide_mode(const char *mode) {
    return !std::strcmp(mode,"idle") || !std::strcmp(mode,"companion");
}
struct Trace {
    uint32_t revision,remaining_ms;
    unsigned step;
    const char *state;
};
Trace trace_locked() {
    const auto &v=model.view();
    return {v.revision,v.remaining_ms,v.step,model.report_state()};
}
void log_transition(const Trace &before,const Trace &after,const char *source) {
    if(before.revision==after.revision)return;
#ifdef ESP_PLATFORM
    ESP_LOGI("TASK_GUIDE","Head %s: state=%s step=%u remaining_ms=%lu",
        source,after.state,after.step+1,(unsigned long)after.remaining_ms);
#else
    (void)source;
#endif
}

bool u32(const cJSON *o,const char *key,uint32_t &result) {
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(o,key);
    if(!cJSON_IsNumber(v) || !std::isfinite(v->valuedouble) || v->valuedouble<0 ||
       v->valuedouble>UINT32_MAX || std::floor(v->valuedouble)!=v->valuedouble) return false;
    result=static_cast<uint32_t>(v->valuedouble);return true;
}
void changed(int64_t now) {
    if(model.view().revision!=model_revision) {
        model_revision=model.view().revision;
        ++report_sequence;last_report_ms=now;
        if(model.view().state==task_guide::State::Idle ||
           model.view().controller_boot_id!=pressed_boot || model.view().request_id!=pressed_request)pressed=false;
    }
}
void centered(uint8_t *bytes,int w,int h,int stride,int y,const char *text,uint16_t color) {
    cjk_font_16_draw_24(bytes,w,h,stride,(w-cjk_font_16_measure(text)*3/2)/2,y,text,color);
}

}

extern "C" void task_guide_set_touch_available(bool available) {
    portENTER_CRITICAL(&guard);touch_available=available;pressed=false;portEXIT_CRITICAL(&guard);
}
extern "C" void task_guide_observe_mode(const cJSON *snapshot) {
    const cJSON *mode=cJSON_GetObjectItemCaseSensitive(snapshot,"mode");
    if(!cJSON_IsString(mode))return;
    const cJSON *guide=cJSON_GetObjectItemCaseSensitive(snapshot,"task_guide");
    const cJSON *state=cJSON_GetObjectItemCaseSensitive(guide,"state");
    uint32_t boot=0,id=0;
    if(!cJSON_IsString(state) || !u32(guide,"controller_boot_id",boot) || !u32(guide,"request_id",id))return;
    const char *steps[3]={nullptr,nullptr,nullptr};
    const cJSON *array=cJSON_GetObjectItemCaseSensitive(guide,"steps");
    if(!std::strcmp(state->valuestring,"ready")) {
        if(!cJSON_IsArray(array)||cJSON_GetArraySize(array)!=3)return;
        for(int i=0;i<3;i++) {
            const cJSON *item=cJSON_GetArrayItem(array,i);
            if(!cJSON_IsString(item))return;
            steps[i]=item->valuestring;
        }
    }
    int64_t now=esp_timer_get_time()/1000;
    const bool allowed=guide_mode(mode->valuestring);
    portENTER_CRITICAL(&guard);
    const Trace before=trace_locked();
    if(!std::strcmp(state->valuestring,"waiting")){waiting_boot=boot;waiting_request=id;notice_until_ms=0;}
    if(!std::strcmp(state->valuestring,"timeout") && waiting_boot==boot && waiting_request==id) {
        notice_until_ms=now+4000;waiting_boot=waiting_request=0;
    }
    model.observe(boot,id,state->valuestring,steps,allowed,now);
    // A failed GT911 registration must not leave a preview that cannot be started/exited.
    bool no_touch=!touch_available && model.view().state==task_guide::State::Preview;
    if(no_touch)model.cancel();
    if(!allowed)notice_until_ms=0;
    changed(now);
    const Trace after=trace_locked();
    portEXIT_CRITICAL(&guard);
    log_transition(before,after,"controller snapshot");
    if(no_touch)ESP_LOGW("TASK_GUIDE","GT911 input unavailable; guide dismissed rather than showing an unusable preview");
}
extern "C" void task_guide_touch_pressed(void) {
    portENTER_CRITICAL(&guard);
    const auto &v=model.view();
    pressed=touch_available && (v.state==task_guide::State::Preview||v.state==task_guide::State::Running);
    pressed_boot=v.controller_boot_id;pressed_request=v.request_id;
    portEXIT_CRITICAL(&guard);
}
extern "C" void task_guide_touch_cancelled(void) {
    portENTER_CRITICAL(&guard);pressed=false;portEXIT_CRITICAL(&guard);
}
extern "C" void task_guide_touch_released(void) {
    const int64_t now=esp_timer_get_time()/1000;
    portENTER_CRITICAL(&guard);
    const Trace before=trace_locked();
    const auto &v=model.view();
    if(pressed && pressed_boot==v.controller_boot_id && pressed_request==v.request_id)model.click(now);
    pressed=false;changed(now);
    const Trace after=trace_locked();
    portEXIT_CRITICAL(&guard);
    log_transition(before,after,"screen release");
}

extern "C" bool task_guide_report(char *json,size_t capacity,uint32_t *sequence) {
    const int64_t now=esp_timer_get_time()/1000;
    task_guide::View view;const char *state=nullptr;
    portENTER_CRITICAL(&guard);
    const Trace before=trace_locked();
    model.update(now);changed(now);
    view=model.view();state=model.report_state();
    if(view.state==task_guide::State::Running && now-last_report_ms>=5000 && acked_sequence==report_sequence) {
        ++report_sequence;last_report_ms=now;
    }
    bool pending=report_sequence!=acked_sequence && view.request_id && state;
    *sequence=report_sequence;
    const Trace after=trace_locked();
    portEXIT_CRITICAL(&guard);
    log_transition(before,after,"local timer");
    if(!pending)return false;
    int n=std::snprintf(json,capacity,"{\"controller_boot_id\":%lu,\"request_id\":%lu,\"state\":\"%s\",\"step\":%u,\"remaining_ms\":%lu,\"report_seq\":%lu}",
        (unsigned long)view.controller_boot_id,(unsigned long)view.request_id,state,view.step,
        (unsigned long)view.remaining_ms,(unsigned long)*sequence);
    return n>0 && static_cast<size_t>(n)<capacity;
}
extern "C" void task_guide_report_ack(uint32_t sequence) {
    portENTER_CRITICAL(&guard);
    if(sequence==report_sequence)acked_sequence=sequence;
    portEXIT_CRITICAL(&guard);
}

bool task_guide_draw(uint8_t *bytes,int width,int height,int stride,int64_t now_ms) {
    task_guide::View view;bool notice;
    portENTER_CRITICAL(&guard);
    const Trace before=trace_locked();
    model.update(now_ms);changed(now_ms);view=model.view();notice=now_ms<notice_until_ms;
    const Trace after=trace_locked();
    portEXIT_CRITICAL(&guard);
    log_transition(before,after,"local timer");
    if(view.state==task_guide::State::Idle&&!notice)return false;
    std::memset(bytes,0,static_cast<size_t>(stride)*height);
    const uint16_t text=pixel_design::rgb565(0xEEF3ED),accent=pixel_design::rgb565(timer_ring::active_rgb);
    if(view.state==task_guide::State::Idle) {
        centered(bytes,width,height,stride,208,"暂时没收到结果",text);
        centered(bytes,width,height,stride,240,"再说一次试试",text);
    } else if(view.state==task_guide::State::Preview) {
        centered(bytes,width,height,stride,64,"三步任务",accent);
        for(int i=0;i<3;i++) {
            // Reserve space for the number and keep long tasks inside the round screen.
            const char *input=view.steps[i];
            char parts[2][TG_STEP_BYTES]={{0},{0}};
            unsigned part=0,count=0;size_t n=0;
            for(const unsigned char *p=reinterpret_cast<const unsigned char *>(input);*p;) {
                if(count==12){part=1;count=0;n=0;}
                unsigned length=*p<0x80?1:(*p<0xE0?2:(*p<0xF0?3:4));
                std::memcpy(parts[part]+n,p,length);n+=length;p+=length;count++;
            }
            char line[80];std::snprintf(line,sizeof(line),"%d. %s",i+1,parts[0]);
            centered(bytes,width,height,stride,128+i*80,line,text);
            if(parts[1][0])centered(bytes,width,height,stride,156+i*80,parts[1],text);
        }
    } else if(view.state==task_guide::State::Done) {
        centered(bytes,width,height,stride,224,"本轮引导结束",accent);
    } else {
        pixel_design::Surface s{bytes,width,height,stride};
        timer_ring::draw_ring(s,view.remaining_ms/180000.f,accent);
        timer_ring::draw_time(s,view.remaining_ms,accent);
        char title[40];std::snprintf(title,sizeof(title),"第%u/3步",view.step+1);
        centered(bytes,width,height,stride,108,title,accent);
        // Balance two lines so a ten-character instruction does not leave one lonely glyph.
        const char *input=view.steps[view.step];char lines[2][TG_STEP_BYTES]={{0},{0}};
        const size_t characters=cjk_font_16_count(input);
        const unsigned split=characters>9?(characters+1)/2:9;
        unsigned line=0,count=0;size_t n=0;
        for(const unsigned char *p=reinterpret_cast<const unsigned char *>(input);*p;) {
            if(count==split){line=1;count=0;n=0;}
            unsigned length=*p<0x80?1:(*p<0xE0?2:(*p<0xF0?3:4));
            std::memcpy(lines[line]+n,p,length);n+=length;p+=length;count++;
        }
        centered(bytes,width,height,stride,300,lines[0],text);
        if(lines[1][0])centered(bytes,width,height,stride,328,lines[1],text);
        centered(bytes,width,height,stride,370,"点击退出",text);
    }
    return true;
}
