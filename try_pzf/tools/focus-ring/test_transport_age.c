#include <assert.h>
#include <stdio.h>
#include "../../shared/focus_transport_age.h"
#include "../../head/main/algo/focus_sync_recovery.h"
#include "../../controller/components/controller_modes/include/focus_clock.h"

static void age_tests(void)
{
    uint32_t age;
    assert(focus_transport_age(2100,1000,1000,&age) && age==100);
    assert(focus_transport_age(6200,1000,5000,&age) && age==200);
    assert(focus_transport_age(2100,1000,0,&age) && age==1100); /* legacy */
    assert(!focus_transport_age(6200,1000,0,&age));
    assert(!focus_transport_age(2100,1000,1101,&age)); /* cannot claim future hold */
    assert(!focus_transport_age(4101,1000,0,&age));
    assert(focus_transport_age(24,UINT32_MAX-25,40,&age) && age==10);
    assert(focus_age_add(UINT32_MAX-20,40)==UINT32_MAX);

    /* Producer already added its own wait. Each echo removes that residence,
     * retaining only two-way link transit. No cross-device clock subtraction. */
    uint32_t sample_age=100;
    assert(focus_transport_age(1080,0,1000,&age));
    sample_age=focus_age_add(sample_age,age); /* 80 ms network, not another 1 s */
    sample_age=focus_age_add(sample_age,500); /* head's real local residence */
    assert(focus_transport_age(560,0,500,&age));
    sample_age=focus_age_add(sample_age,age);
    assert(sample_age==740);
}

static void recovery_tests(void)
{
    focus_sync_recovery_t state={0};
    assert(!focus_sync_failed(&state,0) && state.probe); /* timeout: preserve identities */
    assert(!state.pending_bind);
    state=(focus_sync_recovery_t){0};
    focus_sync_bind_started(&state,3,123);
    assert(focus_sync_bind_confirmed(&state,true,3,456));
    assert(!focus_sync_failed(&state,503) && state.probe && state.pending_bind);
    assert(focus_sync_bind_confirmed(&state,false,3,456)); /* bind succeeded, response lost */
    assert(!focus_sync_bind_confirmed(&state,false,3,123)); /* no proof bind executed */
    assert(!focus_sync_bind_confirmed(&state,false,4,456)); /* source changed during exchange */
    assert(focus_sync_failed(&state,409) && state.probe && !state.pending_bind);
}

static void cadence_tests(unsigned period)
{
    focus_clock_t clock={0};
    focus_clock_start(&clock,120,0);
    /* Test 5/2/1.5 fps model inputs (30-frame classification stride), with
     * one transported result per second. Cached classes cannot confirm debounce. */
    for(unsigned frame=1;frame<=period*2+1;frame++){
        uint32_t link_age, sample_age=100;
        assert(focus_transport_age(1080,0,1000,&link_age));
        sample_age=focus_age_add(sample_age,link_age);
        sample_age=focus_age_add(sample_age,200);
        assert(focus_transport_age(550,0,500,&link_age));
        sample_age=focus_age_add(sample_age,link_age);
        uint32_t inference=(frame-1)/period+1;
        int64_t class_age=(int64_t)((frame-1)%period)*1000000+(int64_t)sample_age*1000;
        assert(focus_clock_sample(&clock,frame,inference,class_age,(int64_t)sample_age*1000,
            true,2,(int64_t)frame*1000000));
        if(frame<=period)assert(clock.rate_half==0);
        else if(class_age>=FOCUS_CLASS_TTL_US)assert(clock.rate_half==0 && clock.reason==FOCUS_STALE);
        else assert(clock.rate_half==2 && clock.reason==FOCUS_RUN);
        if(frame==10){
            focus_sync_recovery_t recovery={0};
            assert(!focus_sync_failed(&recovery,0));
            /* A transient HTTP error freezes transport observation, not the
             * controller's identity, pending classification or elapsed progress. */
            assert(clock.candidate_half==2);
        }
    }
    assert(clock.remaining_us<120000000);
    int64_t expiry=clock.expires_us;
    focus_clock_settle(&clock,expiry+5000000);
    assert(clock.rate_half==0 && clock.reason==FOCUS_STALE);
    int64_t paused=clock.remaining_us;
    focus_clock_settle(&clock,expiry+10000000);
    assert(clock.remaining_us==paused);
}

int main(void)
{
    age_tests();recovery_tests();cadence_tests(6);cadence_tests(15);cadence_tests(20);
    puts("PASS: actual echo-age helper + recovery policy + focus clock: residence once, wrap, invalid hold, TTL, 5/2/1.5fps cadence, no blind rebind");
}
