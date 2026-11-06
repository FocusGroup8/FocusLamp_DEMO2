#include <cstdio>
#include <cassert>
#include "../../head/main/display/focus_ring_renderer.h"
static uint16_t pixels[480*480];

static void write_reminder_frame(const char *directory, const char *name, pixel_design::Surface surface)
{
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%s.ppm", directory, name);
    FILE *file = std::fopen(path, "wb");
    assert(file);
    std::fprintf(file, "P6\n480 480\n255\n");
    const fola::Canvas canvas(surface.bytes, surface.width, surface.height, surface.stride);
    for (int y = 0; y < surface.height; ++y) {
        for (int x = 0; x < surface.width; ++x) {
            const auto colour = canvas.read(x, y);
            const uint8_t rgb[]{colour.r, colour.g, colour.b};
            std::fwrite(rgb, 1, sizeof(rgb), file);
        }
    }
    std::fclose(file);
}

static void verify_reminder(const char *out, pixel_design::Surface surface)
{
    focus_ring::Renderer renderer;
    focus_view_t view{};
    view.focus = view.fresh = view.paused = true;
    view.boot = view.session = view.seq = 1;
    view.duration_ms = 120000;
    view.remaining_ms = 87500;
    std::strcpy(view.reason, "distracted");

    static uint16_t red[480*480], eyes[480*480], prompt[480*480];
    renderer.draw(surface, view, 0);
    std::memcpy(red, pixels, sizeof(pixels));
    renderer.draw(surface, view, 4999, false);
    assert(std::memcmp(red, pixels, sizeof(pixels)) == 0);
    renderer.draw(surface, view, 5000);
    assert(std::memcmp(red, pixels, sizeof(pixels)) != 0);
    std::memcpy(eyes, pixels, sizeof(pixels));
    write_reminder_frame(out, "reminder-eyes", surface);
    ++view.seq;
    renderer.draw(surface, view, 6999);
    assert(std::memcmp(eyes, pixels, sizeof(pixels)) == 0);
    renderer.draw(surface, view, 7000);
    assert(std::memcmp(eyes, pixels, sizeof(pixels)) != 0);
    std::memcpy(prompt, pixels, sizeof(pixels));
    write_reminder_frame(out, "reminder-text", surface);
    std::memset(pixels, 0x5a, sizeof(pixels));
    renderer.draw(surface, view, 8999, false);
    assert(std::memcmp(prompt, pixels, sizeof(pixels)) == 0);
    renderer.draw(surface, view, 9000);
    assert(std::memcmp(red, pixels, sizeof(pixels)) == 0);
    renderer.draw(surface, view, 15000, false);
    assert(std::memcmp(red, pixels, sizeof(pixels)) == 0);

    // Ordinary focus rearms a later reminder. Losing data interrupts the
    // five-second interval and clears a reminder already on screen.
    std::strcpy(view.reason, "running");
    view.paused = false;
    view.rate_quarters = 3;
    renderer.draw(surface, view, 15100);
    std::strcpy(view.reason, "distracted");
    view.paused = true;
    view.rate_quarters = 0;
    renderer.draw(surface, view, 16000);
    view.fresh = false;
    renderer.draw(surface, view, 20000);
    view.fresh = true;
    renderer.draw(surface, view, 20100);
    renderer.draw(surface, view, 25099, false);
    assert(std::memcmp(red, pixels, sizeof(pixels)) == 0);
    renderer.draw(surface, view, 25100);
    assert(std::memcmp(eyes, pixels, sizeof(pixels)) == 0);
    std::strcpy(view.reason, "invalid");
    renderer.draw(surface, view, 25101);
    assert(std::memcmp(eyes, pixels, sizeof(pixels)) != 0);
    std::strcpy(view.reason, "distracted");
    renderer.draw(surface, view, 25200);
    renderer.draw(surface, view, 31000, false);
    assert(std::memcmp(red, pixels, sizeof(pixels)) == 0);

    ++view.session;
    renderer.draw(surface, view, 32000);
    renderer.draw(surface, view, 37000);
    assert(std::memcmp(eyes, pixels, sizeof(pixels)) == 0);
    std::strcpy(view.reason, "running");
    view.paused = false;
    view.rate_quarters = 4;
    renderer.draw(surface, view, 37001);
    assert(std::memcmp(eyes, pixels, sizeof(pixels)) != 0);
}

int main(int argc,char**argv){
    const char *out=argc>1?argv[1]:"tools/focus-ring/previews";
    pixel_design::Surface surface{(uint8_t*)pixels,480,480,960};
    struct Item{const char*name;int rate;const char*reason;bool completed;bool fresh=true;};
    Item states[]={{"green",4,"running",false},{"orange",3,"running",false},{"red",0,"distracted",false},
                   {"waiting",0,"waiting",false},{"voice",0,"voice",false},{"completed",0,"completed",true},
                   {"stale",0,"data_stale",false},{"invalid",0,"invalid",false},
                   {"disconnected",0,"waiting",false,false}};
    for(auto item:states){
        focus_ring::Renderer r;
        focus_view_t v{};v.focus=true;v.fresh=item.fresh;v.boot=1;v.session=1;v.seq=1;
        v.duration_ms=120000;v.remaining_ms=item.completed?0:87500;v.rate_quarters=item.rate;
        v.paused=!item.rate;v.completed=item.completed;snprintf(v.reason,sizeof(v.reason),"%s",item.reason);
        r.draw(surface,v,0);
        // Another renderer may have used the shared canvas. A non-continuing
        // focus frame must restore its entire background/text/track.
        static uint16_t original[480*480];std::memcpy(original,pixels,sizeof(pixels));
        std::memset(pixels,0x5a,sizeof(pixels));r.draw(surface,v,0,false);
        assert(std::memcmp(original,pixels,sizeof(pixels))==0);
        if (item.completed) {
            r.draw(surface,v,420);
            assert(std::memcmp(original,pixels,sizeof(pixels))!=0);
            std::memcpy(original,pixels,sizeof(pixels));
            ++v.seq;
            r.draw(surface,v,500);
            assert(std::memcmp(original,pixels,sizeof(pixels))==0);
            std::memset(pixels,0x5a,sizeof(pixels));
            r.draw(surface,v,700,false);
            assert(std::memcmp(original,pixels,sizeof(pixels))==0);
        }
        char path[512];snprintf(path,sizeof(path),"%s/%s.ppm",out,item.name);
        FILE*f=fopen(path,"wb");assert(f);fprintf(f,"P6\n480 480\n255\n");
        for(auto p:pixels){unsigned char rgb[]={(unsigned char)(((p>>11)&31)*255/31),
            (unsigned char)(((p>>5)&63)*255/63),(unsigned char)((p&31)*255/31)};fwrite(rgb,1,3,f);}fclose(f);
        if(item.completed){
            v.fresh=false;
            r.draw(surface,v,800);
            assert(std::memcmp(original,pixels,sizeof(pixels))!=0);
        } else {
            v.seq++;v.remaining_ms=87000;r.draw(surface,v,500);r.draw(surface,v,650);
            assert(r.shown>=87000 && r.shown<=87500);
            v.seq++;v.paused=true;v.remaining_ms=86900;r.draw(surface,v,700);assert(r.shown==86900);
            v.seq++;v.remaining_ms=86800;v.fresh=false;r.draw(surface,v,1000);
            float frozen=r.shown;r.draw(surface,v,10000);assert(r.shown==frozen);
            v.session++;v.seq=1;v.remaining_ms=120000;r.draw(surface,v,10100);assert(r.shown==120000);
        }
        r.release();assert(!r.initialized);
    }
    verify_reminder(out, surface);
    fola::release_eye_cache();
    puts("PASS: FOLA RGB565 states, completion pop, reminder timing, recovery, rearming and shared canvas restoration");
}
