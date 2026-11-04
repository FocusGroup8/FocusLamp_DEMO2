#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include "../components/task_guide/include/task_guide_gate.h"

int main(void)
{
    assert(tg_request_intent("帮我拆解整理桌面"));
    assert(tg_request_intent("帮我把任务分解成三步"));
    assert(tg_request_intent("拆分一下这个任务"));
    assert(tg_request_intent("清理桌面分3步，提醒我怎么做"));
    assert(tg_request_intent("帮我把清理桌面这个任务分解为3步，然后放在屏幕上提示我"));
    assert(tg_request_intent("请把整理书桌分成三步"));
    assert(tg_request_intent("把任务分为三步放屏幕上"));
    assert(tg_request_intent("帮我把桌面整理的任务拆成三部给我。"));
    assert(tg_request_intent("请把整理桌面的任务分成3部。"));
    assert(!tg_request_intent("不要帮我把任务拆成三部给我。"));
    assert(!tg_request_intent("这个任务是帮我找三部电影。"));
    assert(!tg_request_intent("请把这个任务拆成三部电影。"));
    assert(!tg_request_intent("你把它放在灯头的LCD屏幕里面给我。"));
    assert(!tg_request_intent("不要帮我拆解任务"));
    assert(!tg_request_intent("不用拆解这个任务"));
    assert(!tg_request_intent("拆解这个词是什么意思"));
    assert(!tg_request_intent("分3步是什么意思"));
    assert(!tg_request_intent("为什么把任务分三步"));
    assert(!tg_request_intent("今天心情不错"));
    assert(!tg_request_intent(NULL));

    tg_gate_t g={0};
    tg_gate_mode(&g,123,2,true);
    assert(g.boot==123 && g.state==TG_IDLE);
    assert(tg_gate_request(&g,true,100));
    assert(g.request==1 && g.state==TG_WAITING);
    tg_gate_expire(&g,30099,30000);
    assert(g.state==TG_WAITING);
    tg_gate_expire(&g,30100,30000);
    assert(g.state==TG_TIMEOUT);
    assert(!tg_gate_accept(&g,123,1));

    assert(tg_gate_request(&g,true,31000));
    assert(g.request==2);
    assert(!tg_gate_request(&g,false,31001));
    assert(g.state==TG_DISMISSED);
    assert(!tg_gate_accept(&g,123,2));
    assert(tg_gate_request(&g,true,32000));
    assert(!tg_gate_accept(&g,123,2));
    assert(!tg_gate_accept(&g,124,3));
    assert(tg_gate_accept(&g,123,3));
    assert(!tg_gate_accept(&g,123,3)); /* Tool retries never redispatch ready. */

    assert(!tg_gate_request(&g,false,33000));
    assert(g.state==TG_READY); /* Normal conversation does not stop head timer. */
    assert(!tg_gate_request(&g,true,33001)); /* Cannot overwrite active steps. */
    assert(!tg_gate_report(&g,123,2,1,false,true));
    assert(!tg_gate_report(&g,123,3,0,false,true));
    assert(tg_gate_report(&g,123,3,2,false,false));
    assert(tg_gate_report(&g,123,3,1,false,true));
    assert(g.state==TG_READY && g.report_seq==2); /* Old exit cannot win. */
    assert(tg_gate_report(&g,123,3,3,false,true));
    assert(g.state==TG_DISMISSED);
    assert(tg_gate_report(&g,123,3,4,false,false));
    assert(g.state==TG_DISMISSED); /* Newer stale running cannot revive exit. */
    assert(!tg_gate_accept(&g,123,3));

    assert(tg_gate_request(&g,true,34000));
    assert(tg_gate_accept(&g,123,4));
    assert(tg_gate_report(&g,123,4,1,true,false));
    assert(g.state==TG_DONE);
    assert(tg_gate_request(&g,true,35000));
    tg_gate_mode(&g,123,3,false);
    assert(g.state==TG_DISMISSED);
    assert(!tg_gate_accept(&g,123,5));

    tg_gate_mode(&g,999,1,true); /* Controller reboot is a new identity. */
    assert(g.state==TG_IDLE && g.request==0);
    assert(tg_gate_request(&g,true,36000));
    assert(!tg_gate_accept(&g,123,1));
    assert(tg_gate_accept(&g,999,1));
    puts("task_guide_gate: all assertions passed");
    return 0;
}
