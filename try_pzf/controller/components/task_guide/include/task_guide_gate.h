#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef enum { TG_IDLE, TG_WAITING, TG_READY, TG_TIMEOUT, TG_DISMISSED, TG_DONE } tg_gate_state_t;
typedef struct {
    uint32_t boot, request, mode_revision, report_seq;
    uint64_t opened_ms;
    tg_gate_state_t state;
} tg_gate_t;

/* One authority for late tool results; used unchanged by target and host tests. */
static inline void tg_gate_expire(tg_gate_t *g, uint64_t now, uint32_t timeout_ms)
{
    if (g->state == TG_WAITING && now - g->opened_ms >= timeout_ms) g->state = TG_TIMEOUT;
}
static inline void tg_gate_mode(tg_gate_t *g, uint32_t boot, uint32_t revision, bool ordinary)
{
    if (g->boot != boot) *g = (tg_gate_t){.boot=boot, .mode_revision=revision};
    if (!ordinary || g->mode_revision != revision) {
        if (g->state == TG_WAITING || g->state == TG_READY) g->state = TG_DISMISSED;
        g->mode_revision = revision;
    }
}
static inline bool tg_gate_request(tg_gate_t *g, bool intent, uint64_t now)
{
    /* Ordinary speech cannot stop a running guide. A new pending turn is invalidated. */
    if (g->state == TG_READY) return false;
    if (!intent) { if (g->state == TG_WAITING) g->state = TG_DISMISSED; return false; }
    if (++g->request == 0) ++g->request;
    g->state=TG_WAITING; g->opened_ms=now; g->report_seq=0;
    return true;
}
static inline bool tg_gate_accept(tg_gate_t *g, uint32_t boot, uint32_t request)
{
    if (g->state != TG_WAITING || g->boot != boot || g->request != request) return false;
    g->state=TG_READY; return true;
}
static inline bool tg_gate_report(tg_gate_t *g, uint32_t boot, uint32_t request,
                                  uint32_t seq, bool done, bool dismissed)
{
    if (g->boot!=boot || g->request!=request || !seq) return false;
    if (seq<=g->report_seq) return true; /* Replayed report: ack without overwriting. */
    if (g->state==TG_DONE || g->state==TG_DISMISSED) return true; /* Terminal cannot be revived. */
    if (g->state!=TG_READY && g->state!=TG_WAITING) return false;
    if (g->state==TG_WAITING && !done && !dismissed) return false;
    g->report_seq=seq;
    if (done) g->state=TG_DONE;
    else if (dismissed) g->state=TG_DISMISSED;
    return true;
}
static inline const char *tg_gate_name(tg_gate_state_t state)
{
    static const char *names[]={"idle","waiting","ready","timeout","dismissed","done"};
    return state<=TG_DONE ? names[state] : "idle";
}
/* STT may write the measure word 部 for 步. Only accept it after an explicit
 * task split, with a phrase boundary; 三部电影 and 三部曲 still mean what they say. */
static inline bool tg_asr_task_split(const char *text)
{
    if (!strstr(text, "任务") || strstr(text, "别把") || strstr(text, "不想")) return false;
    static const char *const phrases[] = {"拆成三部", "拆成3部", "分成三部", "分成3部"};
    for (size_t i=0; i<sizeof(phrases)/sizeof(phrases[0]); ++i) {
        const char *match = strstr(text, phrases[i]);
        if (!match) continue;
        const char *tail = match + strlen(phrases[i]);
        if (!*tail || strchr(".,!?", *tail) ||
            strncmp(tail, "。", strlen("。")) == 0 || strncmp(tail, "，", strlen("，")) == 0 ||
            strncmp(tail, "给我", strlen("给我")) == 0) return true;
    }
    return false;
}
static inline bool tg_request_intent(const char *text)
{
    if (!text || strlen(text)>768 || strstr(text,"不要") || strstr(text,"别帮") ||
        strstr(text,"不用") || strstr(text,"不需要")) return false;
    /* These are questions about the wording, not requests to execute a guide. */
    if (strstr(text,"是什么意思") || strstr(text,"这个词") || strstr(text,"为什么") ||
        strstr(text,"举个例子")) return false;
    bool verb=strstr(text,"拆解") || strstr(text,"拆分") || strstr(text,"分解");
    if (verb && (strstr(text,"帮我") || strstr(text,"任务") || strstr(text,"三步") || strstr(text,"3步"))) return true;
    /* A direct "分3步" request is equally explicit; it need not contain 拆解. */
    bool three=strstr(text,"分3步") || strstr(text,"分三步") ||
        strstr(text,"分成3步") || strstr(text,"分成三步") ||
        strstr(text,"分为3步") || strstr(text,"分为三步") ||
        strstr(text,"拆成3步") || strstr(text,"拆成三步");
    return (three || tg_asr_task_split(text)) && (strstr(text,"帮我") || strstr(text,"请") || strstr(text,"提醒我") ||
        strstr(text,"怎么做") || strstr(text,"屏幕") || strstr(text,"任务"));
}
