#include <cstdio>
#include <cassert>
#include "../../head/main/display/focus_ring_renderer.h"
static uint16_t pixels[480*480];
int main(){
    pixel_design::Surface surface{(uint8_t*)pixels,480,480,960};
    struct Item{const char*name;int rate;const char*reason;bool completed;};
    Item states[]={{"green",2,"running",false},{"orange",1,"running",false},{"red",0,"distracted",false},
                   {"waiting",0,"waiting",false},{"voice",0,"voice",false},{"completed",0,"completed",true},
                   {"stale",0,"data_stale",false},{"invalid",0,"invalid",false}};
    for(auto item:states){
        focus_ring::Renderer r;
        focus_view_t v{};v.focus=true;v.fresh=true;v.boot=1;v.session=1;v.seq=1;
        v.duration_ms=120000;v.remaining_ms=item.completed?0:87500;v.rate_half=item.rate;
        v.paused=!item.rate;v.completed=item.completed;snprintf(v.reason,sizeof(v.reason),"%s",item.reason);
        r.draw(surface,v,0);
        char path[160];snprintf(path,sizeof(path),"tools/focus-ring/previews/%s.ppm",item.name);
        FILE*f=fopen(path,"wb");assert(f);fprintf(f,"P6\n480 480\n255\n");
        for(auto p:pixels){unsigned char rgb[]={(unsigned char)(((p>>11)&31)*255/31),
            (unsigned char)(((p>>5)&63)*255/63),(unsigned char)((p&31)*255/31)};fwrite(rgb,1,3,f);}fclose(f);
        if(!item.completed){
            v.seq++;v.remaining_ms=87000;r.draw(surface,v,500);r.draw(surface,v,650);
            assert(r.shown>=87000 && r.shown<=87500);
            v.seq++;v.paused=true;v.remaining_ms=86900;r.draw(surface,v,700);assert(r.shown==86900);
            v.seq++;v.remaining_ms=86800;v.fresh=false;r.draw(surface,v,1000);
            float frozen=r.shown;r.draw(surface,v,10000);assert(r.shown==frozen);
            v.session++;v.seq=1;v.remaining_ms=120000;r.draw(surface,v,10100);assert(r.shown==120000);
        }
    }
    puts("PASS: actual RGB565 renderer, eight states; monotonic follow, pause, disconnect, new session");
}
