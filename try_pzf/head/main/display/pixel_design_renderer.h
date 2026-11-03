#pragma once
#include "pixel_design_data.h"
#include <algorithm>
#include <cmath>
#include <cstring>

// Portable, allocation-free RGB565 renderer. Buffer and stride are supplied by LVGL.
namespace pixel_design {
inline float smooth(float t) {t=std::max(0.f,std::min(1.f,t));return t*t*(3-2*t);}
inline Pose blend(const Pose&a,const Pose&b,float t) {
    return {a.sx+(b.sx-a.sx)*t,a.sy+(b.sy-a.sy)*t,a.upper+(b.upper-a.upper)*t,
      a.lower+(b.lower-a.lower)*t,a.tilt+(b.tilt-a.tilt)*t,a.smile+(b.smile-a.smile)*t,
      a.drop+(b.drop-a.drop)*t,a.coreScale+(b.coreScale-a.coreScale)*t,a.coreY+(b.coreY-a.coreY)*t,
      a.winkL+(b.winkL-a.winkL)*t,a.winkR+(b.winkR-a.winkR)*t,a.mouth+(b.mouth-a.mouth)*t};
}
inline uint16_t rgb565(uint32_t c,float alpha=1) {
    unsigned r=((c>>16)&255)*alpha,g=((c>>8)&255)*alpha,b=(c&255)*alpha;
    return ((r>>3)<<11)|((g>>2)<<5)|(b>>3);
}
inline float snap(float v) {return std::floor(v/2+.5f)*2;}
struct SwitchState {float closed,alpha;bool swapped,done;unsigned frame;};
inline SwitchState switch_state(int role,int64_t elapsed,float initial) {
    const auto&s=roleSwitches[role];float t=std::max(0.f,(float)elapsed),total=s.close+s.hold+s.open,closed;
    if(t<s.close)closed=initial+(1-initial)*smooth(t/s.close);
    else if(t<s.close+s.hold)closed=1;
    else closed=1-smooth((t-s.close-s.hold)/s.open);
    float alpha=t<60?t/60:(t>total-100?(total-t)/100:1);
    return {std::max(0.f,closed),std::max(0.f,std::min(1.f,alpha)),t>=s.close+s.hold/2.f,t>=total,std::min(2u,(unsigned)(t/total*3))};
}
struct Engine {
    int role=-1,requested=0,visible=0;
    uint32_t revision=0,rng=0x7b1d42;
    Pose from=poses[0],to=poses[0];
    int64_t start=0,duration=1,exitAt=0,finishAt=0,habitAt=0,blinkAt=-100000,nextBlink=0;
    bool exiting=false;
    int requestRole=-1,incomingRole=0;
    int64_t switchAt=0;
    float startClosure=0;
    bool switching=false,swapped=false;
    Pose current(int64_t now) const {return blend(from,to,smooth(float(now-start)/duration));}
    void transition(Pose target,int ms,int64_t now){from=current(now);to=target;start=now;duration=std::max(1,ms);}
    void update(int r,int expr,uint32_t rev,int64_t now,bool held=false) {
        if(r!=requestRole || role<0){
          // Keep only the latest destination, freezing the face currently on screen.
          startClosure=role<0?1:closure(now);from=to=current(now);start=now;duration=1;
          if(role<0)role=r;
          requestRole=incomingRole=r;switchAt=now;switching=true;swapped=false;
          requested=expr;revision=rev;exitAt=finishAt=0;habitAt=-100000;blinkAt=now-10000;
        }
        if(switching){
          if(rev!=revision){revision=rev;if(expr!=requested && swapped)transition(poses[expr],profiles[incomingRole].timings[expr].entry,now);requested=expr;}
          const auto state=switch_state(incomingRole,now-switchAt,startClosure);
          if(state.swapped&&!swapped){role=incomingRole;from=to=poses[requested];start=now;duration=1;visible=requested;swapped=true;}
          if(!state.done)return;
          switching=false;visible=requested;
          const auto&t=profiles[role].timings[requested];transition(poses[requested],t.entry?t.entry:240,now);
          exiting=false;exitAt=requested?now+t.entry+t.hold:0;finishAt=requested?exitAt+t.exit:0;
          habitAt=requested?-100000:now;nextBlink=now+profiles[role].blinkMin;
        }
        if(rev!=revision){revision=rev;requested=visible=expr;
          const auto&t=profiles[role].timings[expr];transition(poses[expr],t.entry? t.entry:240,now);
          exiting=false;exitAt=expr?now+t.entry+t.hold:0;finishAt=expr?exitAt+t.exit:0;
          if(expr)habitAt=-100000;
        }
        if(!held&&exitAt&&now>=exitAt&&!exiting){exiting=true;transition(poses[0],profiles[role].timings[requested].exit,now);}
        if(!held&&finishAt&&now>=finishAt){from=to=poses[0];start=now;duration=1;visible=0;exitAt=finishAt=0;}
        if(!visible&&now>=nextBlink){blinkAt=now;rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;
          const auto&p=profiles[role];nextBlink=now+p.blinkMin+rng%(p.blinkMax-p.blinkMin+1);}
    }
    float closure(int64_t now) const {
        if(switching)return switch_state(incomingRole,now-switchAt,startClosure).closed;
        float t=(now-blinkAt)/profiles[role].tempo;
        if(t<0||t>=280)return 0;
        if(t<80)return smooth(t/80);
        if(t<120)return 1;
        return 1-smooth((t-120)/160);
    }
    void habit(int64_t now,float &x,float &closed) const {
        x=closed=0;if(switching)return;
        const auto&p=profiles[role];int64_t t=now-habitAt;
        if(t<0||t>=p.keys[p.keyCount-1].ms)return;
        for(unsigned i=1;i<p.keyCount;i++)if(t<=p.keys[i].ms){auto&a=p.keys[i-1];auto&b=p.keys[i];
          float f=smooth(float(t-a.ms)/(b.ms-a.ms));x=a.x+(b.x-a.x)*f;closed=a.closure+(b.closure-a.closure)*f;return;}
    }
};
struct Surface {
    uint8_t*bytes;int width,height,stride;
    void span(int y,int left,int right,uint16_t color) const {
        if(y<0||y>=height)return;
        left=std::max(0,left);right=std::min(width,right);
        if(right<=left)return;
        auto*row=reinterpret_cast<uint16_t*>(bytes+y*stride);std::fill(row+left,row+right,color);
    }
    void rect(float x,float y,float w,float h,uint16_t color) const {
        int left=std::ceil(x*width/480),right=std::ceil((x+w)*width/480);
        int top=std::ceil(y*height/480),bottom=std::ceil((y+h)*height/480);
        for(int row=std::max(0,top);row<std::min(height,bottom);row++)span(row,left,right,color);
    }
};
inline void render(Surface surface,const Engine&engine,int64_t now,float gazeX,float gazeY) {
    std::memset(surface.bytes,0,surface.stride*surface.height);
    const auto&p=profiles[engine.role];const Pose pose=engine.current(now);
    float habitX,habitClosed;engine.habit(now,habitX,habitClosed);
    const float blink=engine.closure(now);const uint16_t body=rgb565(p.body),core=rgb565(p.light);
    for(int side=-1;side<=1;side+=2){
        float top[18],bottom[18]; // Width <=108, at most18 columns; no heap allocation.
        float closed=std::max(blink,std::max(habitClosed,side>0?pose.winkL:pose.winkR));
        for(int column=0;column<(p.w+5)/6;column++){
            float u=(column*6+3.f)/p.w,inward=side<0?u:1-u;
            float a=std::max(0.f,p.h*(pose.upper+pose.tilt*(inward-.5f)));
            float b=p.h*(1-pose.lower)-pose.smile*p.h*std::sqrt(std::max(0.f,1-(2*u-1)*(2*u-1)));
            float mid=(a+b)/2;top[column]=snap(a+(mid-a)*closed);bottom[column]=snap(b-(b-mid)*closed);
        }
        float cx=240+side*p.gap/2.f,cy=246+(side>0?p.offset:0)+pose.drop;
        float coreW=p.w*p.core*pose.coreScale;
        float coreLeft=snap((p.w-coreW)/2+gazeX+habitX+(engine.role==5?-side*4:-3));
        int y0=std::floor((cy+(-12-p.h/2.f)*pose.sy)*surface.height/480);
        int y1=std::ceil((cy+p.h/2.f*pose.sy)*surface.height/480);
        for(int y=std::max(0,y0);y<std::min(surface.height,y1);y++){
            float localY=((y+.5f)*480/surface.height-cy)/pose.sy+p.h/2.f;
            int index=int(std::floor(localY))+12;if(index<0||index>=p.h+12)continue;
            auto bounds=p.rows[index];float l=bounds.left,r=bounds.right;
            if(side>0){l=p.w-bounds.right;r=p.w-bounds.left;}
            auto fill=[&](float a,float b,uint16_t color){
                int x0=std::ceil((cx+(a-p.w/2.f)*pose.sx)*surface.width/480-.5f);
                int x1=std::ceil((cx+(b-p.w/2.f)*pose.sx)*surface.width/480-.5f);
                surface.span(y,x0,x1,color);
            };
            for(int column=0;column<(p.w+5)/6;column++){
                if(localY<top[column]||localY>=bottom[column])continue;
                float a=std::max(l,float(column*6)),b=std::min(r,float(column*6+6));if(b<=a)continue;
                fill(a,b,body);
                if(localY>=-24+pose.coreY+gazeY&&localY<p.h+24+pose.coreY+gazeY)
                  fill(std::max(a,coreLeft),std::min(b,coreLeft+coreW),core);
            }
        }
        if(engine.role==0){
            float a=std::max(0.f,p.h*(pose.upper-pose.tilt/2)),b=p.h*(1-pose.lower);
            float y=snap(a+(b-a)*closed/2)-12;float x=side<0?-12:p.w-12;
            surface.rect(cx+(x-p.w/2.f)*pose.sx,cy+(y-p.h/2.f)*pose.sy,12*pose.sx,24*pose.sy,rgb565(p.accent));
        }
    }
    const Mouth*mouth=nullptr;unsigned count=0;float alpha=1;
    if(pose.mouth>.05f){mouth=p.surprise;count=p.surpriseCount;alpha=pose.mouth;}
    else if(engine.visible==1){mouth=p.happy;count=p.happyCount;}
    if(engine.switching)alpha*=1-blink;
    for(unsigned i=0;i<count;i++){auto&m=mouth[i];surface.rect(m.x,m.y,m.w,m.h,rgb565(m.color,alpha));}
    if(engine.switching){const auto state=switch_state(engine.incomingRole,now-engine.switchAt,engine.startClosure);
      const auto&icon=roleSwitches[engine.incomingRole];const auto&frame=icon.frames[state.frame];
      if(!state.done&&state.alpha>0)for(unsigned i=0;i<frame.count;i++){const auto&r=frame.rects[i];
        surface.rect(201+r.x*6,66+r.y*6,r.w*6,r.h*6,rgb565(icon.color,state.alpha*(r.tone==1?.68f:1.f)));}
    }
}
}
