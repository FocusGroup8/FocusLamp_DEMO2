/*
 * servo_task.h - 舵机控制任务接口
 */
#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/* 任务入口函数（供 app_tasks.c 任务表引用） */
void servo_task(void *pvParameters);

#ifdef __cplusplus
}
#endif
