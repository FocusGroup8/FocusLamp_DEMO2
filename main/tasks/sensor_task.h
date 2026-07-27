/*
 * sensor_task.h - 传感器采集任务接口
 */
#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/* 任务入口函数（供 app_tasks.c 任务表引用） */
void sensor_task(void *pvParameters);

/* 查询雷达传感器任务是否在运行 */
bool sensor_task_is_running(void);

#ifdef __cplusplus
}
#endif
