#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* User request detection, never inferred from assistant text or facial emotion. */
static inline bool joke_scene_requested(const char *text)
{
    if (!text || !strstr(text,"笑话")) return false;
    const char *negative[]={"不要","别讲","别说","不用","不想","不听","停止","取消","不讲","不说","不是","别再",
        "刚才","什么意思","是什么","解释","讲过","说过","讲的","说的","会讲","会说",
        "别动","不动","只讲","只说"};
    for (unsigned i=0; i<sizeof(negative)/sizeof(*negative); ++i)
        if (strstr(text,negative[i])) return false;
    const char *request[]={"讲个","讲一个","讲一则","讲段","讲笑话","说个","说一个","说一则",
        "来个","来一个","听个","听一个"};
    for (unsigned i=0; i<sizeof(request)/sizeof(*request); ++i)
        if (strstr(text,request[i])) return true;
    return false;
}

typedef struct { bool pending, speaking, fire; int64_t expires_ms; } joke_scene_policy_t;
enum { JOKE_REQUEST=1, JOKE_OTHER, JOKE_TTS_START, JOKE_SENTENCE, JOKE_END };
enum { SCENE_VISION_ACCEPTED=20, SCENE_COMPANION_ENDED, SCENE_DISTRACTION };
static inline bool scene_distraction_due(int64_t now,int64_t last)
{
    return now>=last && now-last>=60000;
}
static inline const char *scene_business_action(int event,bool focus,bool idle)
{
    if (event==SCENE_VISION_ACCEPTED) return "curious";
    if (event==SCENE_COMPANION_ENDED && idle) return "sleepy";
    if (event==SCENE_DISTRACTION && focus) return "angry";
    return NULL;
}
static inline void joke_scene_event(joke_scene_policy_t *s,int event,int64_t now)
{
    if (s->pending && now>s->expires_ms) s->pending=false;
    if (event==JOKE_REQUEST || event==JOKE_OTHER) {
        s->pending=event==JOKE_REQUEST;
        s->fire=false;
        s->speaking=false;
        s->expires_ms=now+30000;
    } else if (event==JOKE_TTS_START) s->speaking=true;
    else if (event==JOKE_SENTENCE && s->pending && s->speaking) {
        s->pending=false; s->fire=true;
    } else if (event==JOKE_END) { s->pending=false; s->speaking=false; s->fire=false; }
}
