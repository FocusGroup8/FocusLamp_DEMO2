#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Relative raw offsets in executor order: EM3(4), LX1, LX2, LX3, LX5.
 * LX5: decreasing raw lowers the head; increasing raw raises it. Each event
 * plays once and returns to its measured starting pose. No home move. */
typedef struct {
    int16_t offset[5];
    uint16_t duration_ms;
    uint16_t hold_ms;
} motion_emotion_step_t;
typedef struct {
    const char *name;
    const motion_emotion_step_t *steps;
    size_t count;
} motion_emotion_plan_t;

static const motion_emotion_step_t motion_happy_steps[] = {
    /* Lift, two upbeat nods, lift/hold, restore. LX5 is the lead axis. */
    {{0,0,-36,-18,300},800,150},
    {{0,0,0,0,-360},650,80},
    {{0,0,-36,-18,300},650,80},
    {{0,0,0,0,-360},650,80},
    {{0,0,-36,-18,300},750,300},
    {{0,0,0,0,0},1000,0},
};
static const motion_emotion_step_t motion_angry_steps[] = {
    /* Same absolute yaw/pitch amplitudes as curious, with refusal rhythm. */
    {{0,0,36,18,-180},900,350},
    {{600,0,36,18,-330},3000,250},
    {{-600,0,36,18,-330},6000,250},
    {{300,0,36,18,-330},4500,250},
    {{0,0,0,0,0},1500,0},
};
static const motion_emotion_step_t motion_curious_steps[] = {
    /* Lift and turn the head, pause to inspect, then raise the gaze slightly. */
    {{0,0,36,18,180},1000,200},
    {{600,0,36,18,180},6000,650},
    {{600,0,36,18,330},800,650},
    {{0,0,0,0,0},6000,0},
};
static const motion_emotion_step_t motion_sleepy_steps[] = {
    /* Slow head droop, brief recovery, deeper droop, restore. */
    {{0,0,36,18,-450},1800,650},
    {{0,0,12,6,-90},1200,250},
    {{0,0,45,21,-570},2000,800},
    {{0,0,0,0,0},1800,0},
};

static inline const motion_emotion_plan_t *motion_emotion_find(const char *name)
{
    static const motion_emotion_plan_t plans[] = {
        {"happy",motion_happy_steps,sizeof(motion_happy_steps)/sizeof(*motion_happy_steps)},
        {"angry",motion_angry_steps,sizeof(motion_angry_steps)/sizeof(*motion_angry_steps)},
        {"curious",motion_curious_steps,sizeof(motion_curious_steps)/sizeof(*motion_curious_steps)},
        {"sleepy",motion_sleepy_steps,sizeof(motion_sleepy_steps)/sizeof(*motion_sleepy_steps)},
    };
    for (size_t i=0; name && i<sizeof(plans)/sizeof(*plans); ++i)
        if (!strcmp(name,plans[i].name)) return &plans[i];
    return NULL;
}

/* Enforced inside the action library, including non-HTTP callers.
 * Legacy intensity fields remain accepted; these three expressions ignore them. */
static inline int motion_emotion_intensity(const char *name, int requested)
{
    if (name && !strcmp(name,"happy")) return 40;
    if (name && (!strcmp(name,"angry") || !strcmp(name,"curious"))) return 50;
    return requested; // Sleepy remains adjustable, default 50 from the executor.
}

/* Whole-plan validation happens before torque or movement. No clipping: an
 * out-of-range sequence must not silently become a different expression. */
static inline int motion_emotion_target(const int16_t start[5],
    const motion_emotion_step_t *step, int intensity, const int minimum[5],
    const int maximum[5], int16_t target[5])
{
    if (intensity<0 || intensity>100) return 0;
    for (int axis=0; axis<5; ++axis) {
        int value=(int)start[axis]+step->offset[axis]*intensity/100;
        if (minimum[axis]>maximum[axis] || start[axis]<minimum[axis] ||
            start[axis]>maximum[axis] || value<minimum[axis] || value>maximum[axis]) return 0;
        target[axis]=(int16_t)value;
    }
    return 1;
}
