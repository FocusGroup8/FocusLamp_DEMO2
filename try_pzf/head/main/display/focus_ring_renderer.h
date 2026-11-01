#pragma once
#include "focus_sync.h"
#include "focus_ring_labels.h"
#include "pixel_design_renderer.h"
#include "timer_ring_renderer.h"
namespace focus_ring {
struct Renderer {
    uint32_t boot=0,session=0,seq=0;
    float shown=0,from=0,target=0;
    int64_t change_ms=0;
    bool initialized=false;
    void draw(pixel_design::Surface surface,const focus_view_t &v,int64_t now) {
        if(!initialized || boot!=v.boot || session!=v.session){
            initialized=true;boot=v.boot;session=v.session;seq=v.seq;
            shown=from=target=v.remaining_ms;change_ms=now;
        }
        if(seq!=v.seq){
            seq=v.seq;from=shown;target=std::min(shown,(float)v.remaining_ms);change_ms=now;
            if(v.paused || v.completed) shown=from=target;
        }
        if(v.fresh && !v.paused && !v.completed){
            float t=std::min(1.f,std::max(0.f,(now-change_ms)/150.f));
            shown=std::min(shown,from+(target-from)*t);
        }
        if(v.completed)shown=0;
        unsigned label=3;uint32_t rgb=0x89939d;
        if(v.completed){label=5;rgb=0x68cf91;}
        else if(!v.fresh){label=7;}
        else if(!strcmp(v.reason,"voice")){label=4;rgb=0x68b7eb;}
        else if(!strcmp(v.reason,"data_stale")){label=6;}
        else if(!strcmp(v.reason,"invalid")){label=8;}
        else if(!strcmp(v.reason,"distracted")){label=2;rgb=0xeb6262;}
        else if(v.rate_half==2){label=0;rgb=0x68cf91;}
        else if(v.rate_half==1){label=1;rgb=0xf0ac52;}
        uint16_t color=pixel_design::rgb565(rgb);
        std::memset(surface.bytes,0,surface.stride*surface.height);
        float fraction=v.duration_ms?shown/v.duration_ms:0;
        timer_ring::draw_ring(surface,fraction,color);
        timer_ring::draw_time(surface,v.remaining_ms,color);
        const auto &text=labels[label];
        for(int column=0;column<text.width;column++)for(int y=0;y<14;y++)
            if(text.columns[column]&(1<<y))surface.rect((480-text.width*2)/2+column*2,302+y*2,2,2,color);
    }
};
}
