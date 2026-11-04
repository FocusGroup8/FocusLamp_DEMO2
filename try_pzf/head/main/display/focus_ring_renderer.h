#pragma once
#include "focus_sync.h"
#include "pixel_design_renderer.h"
#include "fola_eyes.h"
namespace focus_ring {
struct Renderer {
    uint32_t boot=0,session=0,seq=0;
    float shown=0,from=0,target=0;
    int64_t change_ms=0;
    bool initialized=false;
    // Only follows the controller clock; no local prediction.
    void draw(pixel_design::Surface surface,const focus_view_t &view,int64_t now_ms,
              bool continuing=true);
    // Own track only; the animation owner manages the shared eye cache.
    void release();
private:
    fola::Track track_;
    fola::EyeMotion eyes_;
    int state_=-1,track_level_=-1;
    unsigned seconds_=~0u;
    int64_t scene_started_ms_=0;
};
}
