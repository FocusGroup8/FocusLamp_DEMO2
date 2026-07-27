/*
 * radar_data_task.h - 雷达数据采集任务接口
 */

#pragma once
#ifndef __RADAR_DATA_TASK_H__
#define __RADAR_DATA_TASK_H__

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t radar_data_task_start(void);
esp_err_t radar_data_task_stop(void);
bool      radar_data_task_is_running(void);

#ifdef __cplusplus
}
#endif

#endif /* __RADAR_DATA_TASK_H__ */
