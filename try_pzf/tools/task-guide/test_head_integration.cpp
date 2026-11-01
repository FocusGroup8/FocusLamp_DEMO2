#include "task_guide.h"
#include "focus_ring_renderer.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

static int64_t clock_ms=1000;
extern "C" int64_t esp_timer_get_time(void) {return clock_ms*1000;}
static std::vector<uint8_t> pixels(480*480*2);
static bool draw() {return task_guide_draw(pixels.data(),480,480,960,clock_ms);}
static void feed(unsigned request,const char *state,const char *mode="idle") {
    cJSON *o=cJSON_CreateObject(),*g=cJSON_CreateObject();
    cJSON_AddStringToObject(o,"mode",mode);cJSON_AddItemToObject(o,"task_guide",g);
    cJSON_AddNumberToObject(g,"controller_boot_id",17);cJSON_AddNumberToObject(g,"request_id",request);
    cJSON_AddStringToObject(g,"state",state);
    if(!std::strcmp(state,"ready")) {
        const char *steps[]={"丢掉桌面垃圾","把物品分类放好","擦净桌面并摆回常用品"};
        cJSON_AddItemToObject(g,"steps",cJSON_CreateStringArray(steps,3));
    }
    task_guide_observe_mode(o);cJSON_Delete(o);
}
static cJSON *report() {
    char body[400];uint32_t seq;
    assert(task_guide_report(body,sizeof(body),&seq));
    cJSON *j=cJSON_Parse(body);assert(j);task_guide_report_ack(seq);return j;
}
static void expect(const char *state,int step,int remaining) {
    cJSON *j=report();assert(!std::strcmp(cJSON_GetObjectItem(j,"state")->valuestring,state));
    assert(cJSON_GetObjectItem(j,"step")->valueint==step);
    assert(cJSON_GetObjectItem(j,"remaining_ms")->valueint==remaining);cJSON_Delete(j);
}
static void save(const char *name) {
    char path[180];std::snprintf(path,sizeof(path),"tools/task-guide/previews/%s.ppm",name);
    FILE *f=std::fopen(path,"wb");assert(f);std::fprintf(f,"P6\n480 480\n255\n");
    for(size_t i=0;i<pixels.size();i+=2) {
        unsigned p=pixels[i]|(pixels[i+1]<<8);
        unsigned char rgb[]={static_cast<unsigned char>(((p>>11)&31)*255/31),
            static_cast<unsigned char>(((p>>5)&63)*255/63),static_cast<unsigned char>((p&31)*255/31)};
        std::fwrite(rgb,1,3,f);
    }
    std::fclose(f);
}
static void expect_focus_style(uint32_t remaining,bool save_reference=false) {
    std::vector<uint8_t> reference(pixels.size());
    focus_view_t v{};v.fresh=true;v.boot=1;v.session=1;v.seq=1;
    v.duration_ms=180000;v.remaining_ms=remaining;v.rate_half=2;
    std::snprintf(v.reason,sizeof(v.reason),"running");
    focus_ring::Renderer renderer;
    renderer.draw({reference.data(),480,480,960},v,clock_ms);
    // Compare the actual focus and task output, including dark track and every digit pixel.
    for(int y=0;y<480;y++)for(int x=0;x<480;x++) {
        int dx=x-240,dy=y-240;
        if(dx*dx+dy*dy>=160*160 || (y>=200&&y<266&&x>=120&&x<360)) {
            size_t offset=(y*480+x)*2;
            assert(pixels[offset]==reference[offset]&&pixels[offset+1]==reference[offset+1]);
        }
    }
    if(save_reference){pixels.swap(reference);save("focus-reference");pixels.swap(reference);}
}
int main() {
    task_guide_set_touch_available(true);
    feed(0,"idle");feed(1,"waiting");feed(1,"ready");
    assert(draw());save("preview");expect("preview",0,180000);
    // A release without a press cannot start, and the preview never starts its clock.
    clock_ms+=45000;task_guide_touch_released();assert(draw());
    char body[400];uint32_t seq;assert(!task_guide_report(body,sizeof(body),&seq));
    task_guide_touch_pressed();task_guide_touch_released();assert(draw());
    save("step1");expect_focus_style(180000,true);expect("running",0,180000);
    task_guide_touch_released(); // duplicated release must not exit
    clock_ms+=90000;assert(draw());expect_focus_style(90000);save("step1-half");expect("running",0,90000);
    clock_ms+=89999;assert(draw());expect_focus_style(1);expect("running",0,1);
    // A finger held across a step boundary still exits on its genuine release.
    task_guide_touch_pressed();clock_ms+=1;assert(draw());save("step2");expect("running",1,180000);
    task_guide_touch_released();assert(!draw());expect("dismissed",1,0);
    feed(1,"ready");assert(!draw()); // no old response may reopen
    feed(2,"waiting");feed(2,"ready");assert(draw());
    task_guide_touch_pressed();task_guide_touch_released();expect("running",0,180000);
    // No observations/network for the complete nine minutes: local clock remains authoritative.
    clock_ms+=360000;assert(draw());save("step3");expect("running",2,180000);
    clock_ms+=180000;assert(draw());save("done");expect("done",2,0);
    clock_ms+=2000;assert(!draw());feed(2,"ready");assert(!draw());
    task_guide_set_touch_available(false);feed(3,"waiting");feed(3,"ready");assert(!draw());expect("dismissed",0,0);
    task_guide_set_touch_available(true);feed(4,"waiting");feed(4,"ready");assert(draw());
    feed(4,"ready","focus");assert(!draw());expect("dismissed",0,0);
    feed(5,"waiting");feed(5,"timeout");assert(draw());save("timeout");
    clock_ms+=4000;assert(!draw());
    // Optional companion mode still works, while focus and unknown modes cannot show a guide.
    feed(6,"waiting","companion");feed(6,"ready","companion");assert(draw());expect("preview",0,180000);
    feed(6,"ready","unknown");assert(!draw());expect("dismissed",0,0);
    feed(7,"waiting","focus");feed(7,"ready","focus");assert(!draw());
    feed(8,"waiting","unknown");feed(8,"ready","unknown");assert(!draw());
    feed(9,"waiting","idle");feed(9,"ready","idle");assert(draw());expect("preview",0,180000);
    std::puts("PASS: actual idle/companion head JSON integration, focus/unknown rejection, LVGL gesture adapter, offline timer, late result guard and RGB565 screens");
}
