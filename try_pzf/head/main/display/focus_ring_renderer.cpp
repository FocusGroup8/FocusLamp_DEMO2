#include "focus_ring_renderer.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace focus_ring {
namespace {
using namespace fola;
enum State { High, Normal, Distracted, Waiting, Voice, Done, Stale, Disconnected, Invalid };
const char *const labels[]={"高度专注","普通专注","分心暂停","等待专注数据","语音暂停",
                           "已完成","数据过期","连接中断","数据无效"};
constexpr Rgb kText=hex(0xf3f8f9),kMuted=hex(0xcbd6d8),kHint=hex(0x7d8b8e);
const Palette kAmber={hex(0x985b14),hex(0xe39b35),hex(0xf2b95c),
    {hex(0xe9c99b),hex(0xc68c4c),hex(0x90501c)},hex(0xf7d69e),
    {hex(0xffefce),hex(0xf9d9a4),hex(0xf0c582),hex(0xe5b064)},hex(0xfff8e8)};
const Palette kRed={hex(0x972f41),hex(0xcd5366),hex(0xe8818d),
    {hex(0xe5b7bf),hex(0xc57e8b),hex(0x8e4354)},hex(0xf0b8c2),
    {hex(0xffe1e7),hex(0xf5c6cf),hex(0xedacbb),hex(0xdf8b9e)},hex(0xfff0f4)};
const Palette kGrey={hex(0x354a55),hex(0x5c7683),hex(0x8ba1aa),
    {hex(0xc3d0d5),hex(0x92a7b1),hex(0x617d8a)},hex(0xb3c5ce),
    {hex(0xe4edf0),hex(0xcad8df),hex(0xb2c5d0),hex(0x96b0bf)},hex(0xf0f6f8)};
const TrackStyle kAmberTrack={hex(0x263139),.86f,hex(0xb76b20),hex(0xe5a34b),hex(0xf5ce84),hex(0xe5a34b),hex(0xffe6b4)};
const TrackStyle kRedTrack={hex(0x263139),.86f,hex(0xae3e53),hex(0xdc6474),hex(0xf19aa6),hex(0xdc6474),hex(0xffd5dc)};
const TrackStyle kGreyTrack={hex(0x263139),.86f,hex(0x667985),hex(0x8a9fa9),hex(0xb6c7ce),hex(0x8a9fa9),hex(0xe1eaef)};
const TrackStyle kVoiceTrack={hex(0x263139),.86f,hex(0x3b8bb0),hex(0x69bbdc),hex(0xa3deed),hex(0x69bbdc),hex(0xd5f4ff)};
State state_for(const focus_view_t &v) {
    if(v.completed)return Done;
    if(!v.fresh)return Disconnected;
    if(!std::strcmp(v.reason,"voice"))return Voice;
    if(!std::strcmp(v.reason,"data_stale"))return Stale;
    if(!std::strcmp(v.reason,"invalid"))return Invalid;
    if(!std::strcmp(v.reason,"distracted"))return Distracted;
    return v.rate_quarters==4?High:v.rate_quarters>0?Normal:Waiting;
}
const TrackStyle &style_for(State s) {
    return s==High?kGreenTrack:s==Normal?kAmberTrack:s==Distracted?kRedTrack:s==Voice?kVoiceTrack:kGreyTrack;
}
EyeLayout layout_for(State s) {
    if(s==Done)return {&kLavender,.85f,240,196};
    return {s==High?&kGreen:s==Normal?&kAmber:s==Distracted?&kRed:s==Voice?&kTeal:&kGrey,
            .70f,240,168,true};
}
EyePose pose_for(State s) {
    EyePose p;
    if(s==Done)p.left.happy=p.right.happy=true;
    else if(s==Distracted){p.left.lid=p.right.lid=.08f;p.left.tilt=p.right.tilt=-.12f;}
    return p;
}
}
void Renderer::release() {
    track_.release();initialized=false;state_=-1;track_level_=-1;seconds_=~0u;
}
void Renderer::draw(pixel_design::Surface surface,const focus_view_t &v,int64_t now,bool continuing) {
    const bool new_session=!initialized||boot!=v.boot||session!=v.session;
    if(new_session){
        initialized=true;boot=v.boot;session=v.session;seq=v.seq;
        shown=from=target=v.remaining_ms;change_ms=now;
    }
    if(seq!=v.seq){
        seq=v.seq;from=shown;target=std::min(shown,(float)v.remaining_ms);change_ms=now;
        if(v.paused||v.completed)shown=from=target;
    }
    if(v.fresh&&!v.paused&&!v.completed){
        float t=std::min(1.f,std::max(0.f,(now-change_ms)/150.f));
        shown=std::min(shown,from+(target-from)*t);
    }
    if(v.completed)shown=0;
    const State state=state_for(v);
    const fola::Canvas c(surface.bytes,surface.width,surface.height,surface.stride);
    const bool full=new_session||!continuing||state_!=state;
    if(full){
        state_=state;track_level_=-1;seconds_=~0u;scene_started_ms_=now;
        c.background(c.bounds());
        eyes_.reset(pose_for(state),now);eyes_.glances=state==High||state==Normal;
        fola::text_centered(c,240,state==Done?342:292,labels[state],state==Done?1.5f:2.f,kText);
        if(state!=Done)fola::text_centered(c,240,348,"剩余时间",1.f,kHint);
    }
    if(state!=Done){
        const float fraction=v.duration_ms?std::max(0.f,std::min(1.f,shown/v.duration_ms)):0.f;
        const int level=int(fraction*2048);
        if(full||track_level_!=level){track_level_=level;track_.draw(c,fraction,style_for(state));}
    }
    const unsigned seconds=(v.remaining_ms+999)/1000;
    if(full||seconds_!=seconds){
        seconds_=seconds;
        if(!full)c.background(c.to_pixels(140,374,200,24));
        char time[20];std::snprintf(time,sizeof(time),"%u:%02u",seconds/60,seconds%60);
        fola::text_centered(c,240,374,time,1.5f,kMuted,.85f);
    }
    auto pose=eyes_.pose(now);
    if(state==Done){
        const float t=float(now-scene_started_ms_);
        if(t<700)pose.lift=-fola::kMaxLift*std::fabs(std::sin(t/350.f*3.1415927f));
    }
    fola::draw_eyes(c,layout_for(state),pose);
    if(state==Done){
        const float twinkle=.55f+.45f*std::sin(float(now-scene_started_ms_)/260.f);
        fola::sparkle(c,88,150,16,fola::hex(0xe6dbef),twinkle);
        fola::sparkle(c,396,236,12,fola::hex(0xe6dbef),1.1f-twinkle);
    }
}
}
