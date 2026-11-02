#include <assert.h>
#include <stdio.h>
#include "../../controller/components/controller_modes/include/focus_clock.h"
#define S(x) ((int64_t)(x)*1000000)
static void ready(focus_clock_t *c,int rate) {
    focus_clock_start(c,120,0);
    assert(focus_clock_sample(c,1,1,0,0,true,rate,S(1)));
    assert(focus_clock_sample(c,2,2,0,0,true,rate,S(2)));
    assert(c->rate_quarters==rate);
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
        assert(focus_clock_sample(&c,sample,inference,class_age,200000,true,4,S(sample)));
        if(sample<=period)assert(c.rate_quarters==0);
        else if(class_age>=FOCUS_CLASS_TTL_US) {
            assert(c.rate_quarters==0 && c.reason==FOCUS_STALE);
            if(previous_stale)assert(c.remaining_us==previous_remaining);
        } else assert(c.rate_quarters==4 && c.reason==FOCUS_RUN);
        assert(c.candidate_quarters==4); /* Never count until a second fresh class. */
        assert(c.remaining_us<=previous_remaining);
        previous_remaining=c.remaining_us;
        previous_stale=class_age>=FOCUS_CLASS_TTL_US;
    }
    assert(c.remaining_us<S(120));
}
static void true_sample_gap_and_invalid_reset(void) {
    focus_clock_t c={0};
    focus_clock_start(&c,120,0);
    focus_clock_sample(&c,1,1,0,0,true,4,S(1));
    assert(c.candidate_quarters==4 && c.rate_quarters==0);
    focus_clock_settle(&c,S(10)); /* No fresh samples for longer than 3 s. */
    assert(c.candidate_quarters==-1 && c.reason==FOCUS_STALE);
    focus_clock_sample(&c,2,2,0,0,true,4,S(10));
    assert(c.candidate_quarters==4 && c.rate_quarters==0);
    focus_clock_sample(&c,3,2,S(1),0,true,4,S(11));
    assert(c.rate_quarters==0); /* A cached class cannot finish a new debounce. */
    focus_clock_sample(&c,4,3,0,0,true,4,S(12));
    assert(c.rate_quarters==4);
    focus_clock_sample(&c,5,3,S(1),0,false,-1,S(13));
    assert(c.candidate_quarters==-1 && c.reason==FOCUS_INVALID && c.rate_quarters==0);
    focus_clock_sample(&c,6,4,0,0,true,4,S(14));
    assert(c.rate_quarters==0); /* Invalid data cannot bridge confirmations. */
    focus_clock_sample(&c,7,5,0,0,true,4,S(15));
    assert(c.rate_quarters==4);
    focus_clock_sample(&c,8,6,0,FOCUS_SAMPLE_TTL_US,true,4,S(16));
    assert(c.candidate_quarters==-1 && c.rate_quarters==0 && c.reason==FOCUS_STALE);
}
static void three_quarter_tests(void) {
    focus_clock_t c={0}, single={0};
    ready(&c,3); single=c;
    /* Four tiny updates must equal one update: do not lose fractional us. */
    c.expires_us=single.expires_us=S(1000);
    for(int i=1;i<=4;i++)focus_clock_settle(&c,S(2)+i);
    focus_clock_settle(&single,S(2)+4);
    assert(c.remaining_us==S(120)-3 && c.remaining_us==single.remaining_us);
    assert(c.debit_remainder==single.debit_remainder && !c.debit_remainder);

    ready(&c,3);
    focus_clock_sample(&c,3,3,0,0,true,4,S(3));
    focus_clock_sample(&c,4,4,0,0,true,4,S(4));
    focus_clock_sample(&c,5,5,0,0,true,4,S(5));
    assert(c.remaining_us==S(120)-2500000); /* old .75 settled before 1x */
    assert(c.normal_us==S(2) && c.full_us==S(1));

    ready(&c,3);focus_clock_voice(&c,true,S(3));
    for(int i=4;i<=13;i++)focus_clock_sample(&c,i,i,0,0,true,3,S(i));
    assert(c.remaining_us==S(120)-750000);
    focus_clock_voice(&c,false,S(13));focus_clock_settle(&c,S(15));
    assert(c.remaining_us==S(120)-2250000 && c.held_us[FOCUS_VOICE]==S(10));

    ready(&c,3);focus_clock_settle(&c,S(20));
    assert(c.remaining_us==S(120)-2250000 && c.reason==FOCUS_STALE);
    ready(&c,3);
    for(int i=3;i<=162;i++)focus_clock_sample(&c,i,i,0,0,true,3,S(i));
    assert(c.completed && c.active && c.remaining_us==0 && c.normal_us==S(160));
    focus_clock_settle(&c,S(500));assert(c.normal_us==S(160));
}
int main(void) {
    three_quarter_tests();
    /* Residence is partitioned at expiry; delayed polling cannot debit stale time. */
    focus_clock_t totals={0};
    ready(&totals,4);ten_seconds(&totals,4);
    assert(totals.full_us==S(10) && totals.held_us[FOCUS_WAIT]==S(2));
    ready(&totals,3);ten_seconds(&totals,3);
    assert(totals.normal_us==S(10) && totals.remaining_us==(S(120)-S(10)*3/4));
    ready(&totals,0);ten_seconds(&totals,0);
    assert(totals.held_us[FOCUS_DISTRACTED]==S(10));
    ready(&totals,4);
    focus_clock_sample(&totals,3,3,0,0,true,3,S(3));
    focus_clock_sample(&totals,4,4,0,0,true,3,S(4));
    focus_clock_sample(&totals,5,5,0,0,true,3,S(5));
    assert(totals.full_us==S(2) && totals.normal_us==S(1));
    assert(totals.remaining_us==S(120)-S(2)-S(1)*3/4);
    focus_clock_sample(&totals,6,6,0,0,false,-1,S(6));
    focus_clock_settle(&totals,S(8));
    assert(totals.held_us[FOCUS_INVALID]==S(2));
    ready(&totals,4);focus_clock_settle(&totals,S(10));
    assert(totals.full_us==S(3) && totals.held_us[FOCUS_STALE]==S(5));
    focus_clock_settle(&totals,S(10));assert(totals.held_us[FOCUS_STALE]==S(5));
    ready(&totals,4);focus_clock_voice(&totals,true,S(3));
    focus_clock_settle(&totals,S(10));
    assert(totals.full_us==S(1) && totals.held_us[FOCUS_VOICE]==S(7));
    totals.voice=false;ready(&totals,4);totals.remaining_us=S(1);focus_clock_settle(&totals,S(10));
    assert(totals.completed && totals.full_us==S(1));
    focus_clock_settle(&totals,S(20));assert(totals.full_us==S(1));
    focus_clock_start(&totals,120,S(21));assert(!totals.full_us && !totals.normal_us && !totals.held_us[FOCUS_VOICE]);
    slow_classification_cadence(15);
    slow_classification_cadence(20);
    true_sample_gap_and_invalid_reset();
    focus_clock_t c={0};
    ready(&c,4);ten_seconds(&c,4);assert(c.remaining_us==S(110));
    ready(&c,3);ten_seconds(&c,3);assert(c.remaining_us==(S(120)-S(10)*3/4));
    ready(&c,0);ten_seconds(&c,0);assert(c.remaining_us==S(120));
    ready(&c,4);
    int64_t deadline=c.expires_us;
    assert(!focus_clock_sample(&c,2,2,0,0,true,4,S(3)));
    assert(c.expires_us==deadline);
    focus_clock_settle(&c,S(20));assert(c.remaining_us==S(117));assert(c.reason==FOCUS_STALE);
    focus_clock_settle(&c,S(21));assert(c.remaining_us==S(117)); // no negative delta
    ready(&c,4);focus_clock_voice(&c,true,S(3));assert(c.remaining_us==S(119));
    ten_seconds(&c,4);assert(c.remaining_us==S(119));
    focus_clock_voice(&c,false,S(12));focus_clock_settle(&c,S(13));assert(c.remaining_us==S(118));
    ready(&c,4);focus_clock_sample(&c,3,2,S(1),0,false,-1,S(3));
    assert(c.reason==FOCUS_INVALID && c.remaining_us==S(119));
    focus_clock_sample(&c,4,2,S(2),0,true,4,S(4));assert(c.rate_quarters==0);
    focus_clock_sample(&c,5,3,0,0,true,4,S(5));assert(c.rate_quarters==0);
    focus_clock_sample(&c,6,4,0,0,true,4,S(6));assert(c.rate_quarters==4);
    // Old classifications in new frames cannot satisfy the debounce.
    focus_clock_start(&c,120,0);
    focus_clock_sample(&c,1,1,0,0,true,4,S(1));
    focus_clock_sample(&c,2,1,S(1),0,true,4,S(2));assert(c.rate_quarters==0);
    // Class TTL cannot be extended by repeated classifications or fresh faces.
    ready(&c,4);
    for(int i=3;i<17;i++)focus_clock_sample(&c,i,2,S(i-2),0,true,4,S(i));
    focus_clock_settle(&c,S(50));assert(c.remaining_us==S(105));
    // Same request carrying a pre-entry inference does not start a session.
    focus_clock_start(&c,120,S(50));
    focus_clock_sample(&c,100,10,S(3),0,true,4,S(51));assert(c.rate_quarters==0);
    assert(c.remaining_us==S(120));
    uint32_t session=c.session;focus_clock_start(&c,120,S(52));assert(c.session==session+1);
    ready(&c,4);
    for(int i=3;i<=122;i++)focus_clock_sample(&c,i,i,0,0,true,4,S(i));
    assert(c.completed && c.active && c.remaining_us==0);
    focus_clock_settle(&c,S(500));assert(c.completed && c.remaining_us==0);
    focus_clock_start(&c,120,S(501));assert(!c.completed && c.remaining_us==S(120) && c.rate_quarters==0);
    // Binding/reconnecting pauses without resetting remaining time.
    ready(&c,4);focus_clock_bind(&c,S(3));assert(c.remaining_us==S(119) && c.rate_quarters==0);
    puts("PASS: actual focus clock: 1x/.75x/0; confirmed-state 10s -> remaining 110000/112500/120000ms; 160s at .75x completes 120s target; fractional us preserved; class/sample TTL, repeated data, voice, session and completion guards");
}
