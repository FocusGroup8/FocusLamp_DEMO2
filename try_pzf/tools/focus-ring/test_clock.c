#include <assert.h>
#include <stdio.h>
#include "../../controller/components/controller_modes/include/focus_clock.h"
#define S(x) ((int64_t)(x)*1000000)
static void ready(focus_clock_t *c,int rate) {
    focus_clock_start(c,120,0);
    assert(focus_clock_sample(c,1,1,0,0,true,rate,S(1)));
    assert(focus_clock_sample(c,2,2,0,0,true,rate,S(2)));
    assert(c->rate_half==rate);
}
static void ten_seconds(focus_clock_t *c,int rate) {
    for(int i=3;i<=12;i++)focus_clock_sample(c,i,i,0,0,true,rate,S(i));
}
static void slow_classification_cadence(unsigned period) {
    focus_clock_t c={0};
    focus_clock_start(&c,120,0);
    int64_t previous_remaining=c.remaining_us;
    bool previous_stale=false;
    /* 30-frame classifications at 2 fps (15 s) or 1.5 fps (20 s).
     * Fresh face samples still reach the controller once per second. */
    for(unsigned sample=1;sample<=period*2+1;sample++) {
        unsigned inference=(sample-1)/period+1;
        int64_t class_age=S((sample-1)%period)+200000;
        assert(focus_clock_sample(&c,sample,inference,class_age,200000,true,2,S(sample)));
        if(sample<=period)assert(c.rate_half==0);
        else if(class_age>=FOCUS_CLASS_TTL_US) {
            assert(c.rate_half==0 && c.reason==FOCUS_STALE);
            if(previous_stale)assert(c.remaining_us==previous_remaining);
        } else assert(c.rate_half==2 && c.reason==FOCUS_RUN);
        assert(c.candidate_half==2); /* Never count until a second fresh class. */
        assert(c.remaining_us<=previous_remaining);
        previous_remaining=c.remaining_us;
        previous_stale=class_age>=FOCUS_CLASS_TTL_US;
    }
    assert(c.remaining_us<S(120));
}
static void true_sample_gap_and_invalid_reset(void) {
    focus_clock_t c={0};
    focus_clock_start(&c,120,0);
    focus_clock_sample(&c,1,1,0,0,true,2,S(1));
    assert(c.candidate_half==2 && c.rate_half==0);
    focus_clock_settle(&c,S(10)); /* No fresh samples for longer than 3 s. */
    assert(c.candidate_half==-1 && c.reason==FOCUS_STALE);
    focus_clock_sample(&c,2,2,0,0,true,2,S(10));
    assert(c.candidate_half==2 && c.rate_half==0);
    focus_clock_sample(&c,3,2,S(1),0,true,2,S(11));
    assert(c.rate_half==0); /* A cached class cannot finish a new debounce. */
    focus_clock_sample(&c,4,3,0,0,true,2,S(12));
    assert(c.rate_half==2);
    focus_clock_sample(&c,5,3,S(1),0,false,-1,S(13));
    assert(c.candidate_half==-1 && c.reason==FOCUS_INVALID && c.rate_half==0);
    focus_clock_sample(&c,6,4,0,0,true,2,S(14));
    assert(c.rate_half==0); /* Invalid data cannot bridge confirmations. */
    focus_clock_sample(&c,7,5,0,0,true,2,S(15));
    assert(c.rate_half==2);
    focus_clock_sample(&c,8,6,0,FOCUS_SAMPLE_TTL_US,true,2,S(16));
    assert(c.candidate_half==-1 && c.rate_half==0 && c.reason==FOCUS_STALE);
}
int main(void) {
    slow_classification_cadence(15);
    slow_classification_cadence(20);
    true_sample_gap_and_invalid_reset();
    focus_clock_t c={0};
    ready(&c,2);ten_seconds(&c,2);assert(c.remaining_us==S(110));
    ready(&c,1);ten_seconds(&c,1);assert(c.remaining_us==S(115));
    ready(&c,0);ten_seconds(&c,0);assert(c.remaining_us==S(120));
    ready(&c,2);
    int64_t deadline=c.expires_us;
    assert(!focus_clock_sample(&c,2,2,0,0,true,2,S(3)));
    assert(c.expires_us==deadline);
    focus_clock_settle(&c,S(20));assert(c.remaining_us==S(117));assert(c.reason==FOCUS_STALE);
    focus_clock_settle(&c,S(21));assert(c.remaining_us==S(117)); // no negative delta
    ready(&c,2);focus_clock_voice(&c,true,S(3));assert(c.remaining_us==S(119));
    ten_seconds(&c,2);assert(c.remaining_us==S(119));
    focus_clock_voice(&c,false,S(12));focus_clock_settle(&c,S(13));assert(c.remaining_us==S(118));
    ready(&c,2);focus_clock_sample(&c,3,2,S(1),0,false,-1,S(3));
    assert(c.reason==FOCUS_INVALID && c.remaining_us==S(119));
    focus_clock_sample(&c,4,2,S(2),0,true,2,S(4));assert(c.rate_half==0);
    focus_clock_sample(&c,5,3,0,0,true,2,S(5));assert(c.rate_half==0);
    focus_clock_sample(&c,6,4,0,0,true,2,S(6));assert(c.rate_half==2);
    // Old classifications in new frames cannot satisfy the debounce.
    focus_clock_start(&c,120,0);
    focus_clock_sample(&c,1,1,0,0,true,2,S(1));
    focus_clock_sample(&c,2,1,S(1),0,true,2,S(2));assert(c.rate_half==0);
    // Class TTL cannot be extended by repeated classifications or fresh faces.
    ready(&c,2);
    for(int i=3;i<17;i++)focus_clock_sample(&c,i,2,S(i-2),0,true,2,S(i));
    focus_clock_settle(&c,S(50));assert(c.remaining_us==S(105));
    // Same request carrying a pre-entry inference does not start a session.
    focus_clock_start(&c,120,S(50));
    focus_clock_sample(&c,100,10,S(3),0,true,2,S(51));assert(c.rate_half==0);
    assert(c.remaining_us==S(120));
    uint32_t session=c.session;focus_clock_start(&c,120,S(52));assert(c.session==session+1);
    ready(&c,2);
    for(int i=3;i<=122;i++)focus_clock_sample(&c,i,i,0,0,true,2,S(i));
    assert(c.completed && c.active && c.remaining_us==0);
    focus_clock_settle(&c,S(500));assert(c.completed && c.remaining_us==0);
    focus_clock_start(&c,120,S(501));assert(!c.completed && c.remaining_us==S(120) && c.rate_half==0);
    // Binding/reconnecting pauses without resetting remaining time.
    ready(&c,2);focus_clock_bind(&c,S(3));assert(c.remaining_us==S(119) && c.rate_half==0);
    puts("PASS: actual focus clock: 2fps/1.5fps classifications, class expiry pauses, actual sample gaps, rates, deadline, repeats, voice, invalid, session, completion, bind");
}
