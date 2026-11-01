#pragma once
#include "pixel_design_renderer.h"

// Shared visual primitives: keep focus mode and task guidance on the same clock face.
// These functions only draw. Each caller retains its own timing/state authority.
namespace timer_ring {
static constexpr uint32_t active_rgb=0x68cf91;
static constexpr uint32_t track_rgb=0x202830;

inline void draw_ring(pixel_design::Surface surface,float fraction,uint16_t color) {
    const uint16_t track=pixel_design::rgb565(track_rgb);
    // Original focus clock: 72 blocks, radius 180, snapped to the 12px grid.
    for(int i=0;i<72;i++) {
        float a=i*6.2831853f/72-1.5707963f;
        float x=std::round((240+180*std::cos(a)-6)/12)*12;
        float y=std::round((240+180*std::sin(a)-6)/12)*12;
        surface.rect(x,y,12,12,i<72*fraction?color:track);
    }
}

inline void draw_time(pixel_design::Surface surface,uint32_t remaining_ms,uint16_t color) {
    static constexpr unsigned masks[]={0x3f,0x06,0x5b,0x4f,0x66,0x6d,0x7d,0x07,0x7f,0x6f};
    unsigned seconds=(remaining_ms+999)/1000;
    unsigned minutes=seconds/60;
    // Preserve the original MM:SS layout, and the third minute digit for long sessions.
    unsigned digits[]={minutes/100,(minutes/10)%10,minutes%10,(seconds/10)%6,seconds%10};
    int first=minutes>=100?0:1,count=5-first;
    int x=(480-(count*48+24))/2;
    for(int i=first;i<5;i++) {
        if(i==3){surface.rect(x,215,8,8,color);surface.rect(x,239,8,8,color);x+=24;}
        unsigned bits=masks[digits[i]];const int y=200;
        if(bits&1)surface.rect(x+6,y,24,6,color);
        if(bits&2)surface.rect(x+30,y+6,6,24,color);
        if(bits&4)surface.rect(x+30,y+36,6,24,color);
        if(bits&8)surface.rect(x+6,y+60,24,6,color);
        if(bits&16)surface.rect(x,y+36,6,24,color);
        if(bits&32)surface.rect(x,y+6,6,24,color);
        if(bits&64)surface.rect(x+6,y+30,24,6,color);
        x+=48;
    }
}
}
