#include <assert.h>
#include <stdio.h>
#include "../../controller/components/controller_modes/src/controller_modes.c"
int64_t fake_now;
static controller_modes_status_t state(void){controller_modes_status_t s;assert(controller_modes_get_status(&s)==ESP_OK);return s;}
static void report(unsigned seq){controller_modes_status_t s=state();assert(controller_modes_engagement(s.controller_boot_id,s.session_id,s.engagement_token,false,seq,seq,0,0,true,"HighlyEngaged")==ESP_OK);}
static void bind(void){controller_modes_status_t s=state();assert(controller_modes_engagement(s.controller_boot_id,s.session_id,0,true,0,0,0,0,false,"")==ESP_OK);}
int main(void){
  assert(engagement_rate("HighlyEngaged")==4 && engagement_rate("highly_focus")==4);
  assert(engagement_rate("Engaged")==3 && engagement_rate("focus")==3);
  assert(engagement_rate("BarelyEngaged")==0 && engagement_rate("NotEngaged")==0);
  assert(engagement_rate("tired")==0 && engagement_rate("Unknown")==-1);
  assert(controller_modes_init()==ESP_OK);
  assert(controller_modes_focus_start(2)==ESP_OK);controller_modes_status_t a=state();assert(a.remaining_ms==120000 && a.paused);
  assert(controller_modes_focus_start(2)==ESP_ERR_INVALID_STATE);assert(state().session_id==a.session_id);
  bind();for(unsigned i=1;i<=121;i++){fake_now=(int64_t)i*1000000;report(i);}
  fake_now=122000000;controller_modes_status_t done=state();assert(done.mode==APP_STATE_FOCUS && done.completed && !done.remaining_ms);
  unsigned revision=done.revision;
  fake_now+=FOCUS_COMPLETE_HOLD_US-1;assert(state().completed && state().revision==revision);
  fake_now+=1;timer_callback(NULL);a=state();assert(a.mode==APP_STATE_IDLE && !a.completed && !a.duration_ms && !a.remaining_ms && !a.engagement_token);
  assert(a.revision==revision+1);revision=a.revision;
  fake_now+=5000000;assert(state().revision==revision); /* Exit happens once. */
  assert(controller_modes_focus_start(3)==ESP_OK);a=state();assert(!a.completed && a.remaining_ms==180000 && a.session_id!=done.session_id);
  assert(controller_modes_engagement(a.controller_boot_id,done.session_id,done.engagement_token,false,500,500,0,0,true,"HighlyEngaged")==ESP_ERR_INVALID_STATE);
  assert(controller_modes_focus_stop()==ESP_OK);assert(state().mode==APP_STATE_IDLE && !state().completed);
  controller_modes_set_voice_active(true);assert(controller_modes_focus_start(2)==ESP_OK);assert(state().paused && !strcmp(controller_modes_pause_name(state().pause_reason),"voice"));
  controller_modes_set_voice_active(false);assert(state().remaining_ms==120000);
  assert(controller_modes_companion_start()==ESP_OK);a=state();assert(controller_modes_focus_start(1)==ESP_OK);assert(state().session_id==a.session_id+1);
  bind();unsigned i=1;
  for(;i<=12;i++){
    fake_now+=1000000;a=state();
    assert(controller_modes_engagement(a.controller_boot_id,a.session_id,a.engagement_token,false,i,i,0,0,true,"Engaged")==ESP_OK);
  }
  a=state();assert(a.rate_quarters==3 && a.rate_half==1.5 && a.remaining_ms==52500 && a.normal_ms==10000 && !a.half_ms);
  assert(controller_modes_focus_stop()==ESP_OK);
  /* A new session or mode during the completion indication must cancel exit. */
  assert(controller_modes_focus_start(1)==ESP_OK);bind();
  for(i=1;i<=61;i++){fake_now+=1000000;report(i);}
  fake_now+=1000000;
  a=state();assert(a.completed);
  unsigned previous_session=a.session_id;
  assert(controller_modes_focus_start(1)==ESP_OK);
  fake_now+=FOCUS_COMPLETE_HOLD_US+1;
  a=state();assert(a.mode==APP_STATE_FOCUS && !a.completed && a.session_id==previous_session+1 && a.remaining_ms==60000);
  bind();for(i=1;i<=61;i++){fake_now+=1000000;report(i);}
  fake_now+=1000000;
  assert(state().completed);
  assert(controller_modes_companion_start()==ESP_OK);
  fake_now+=FOCUS_COMPLETE_HOLD_US+1;
  a=state();assert(a.mode==APP_STATE_COMPANION && !a.completed);
  assert(controller_modes_focus_stop()==ESP_OK);assert(state().mode==APP_STATE_COMPANION);
  assert(controller_modes_stop()==ESP_OK);
  puts("PASS: actual controller_modes APIs: duplicate start, 2s completion then idle once, restart/mode change cancels old exit, session guard, stop, voice, .75x");
}

