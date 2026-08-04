/*
 * stage1_validation_task.h - Stage1 硬件验证任务接口
 *
 * 执行雷达模块和硬件功能验证。
 */

#pragma once
#ifndef __STAGE1_VALIDATION_TASK_H__
#define __STAGE1_VALIDATION_TASK_H__

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t stage1_validation_task_start(void);
esp_err_t stage1_validation_task_stop(void);
bool      stage1_validation_task_is_running(void);

#ifdef __cplusplus
}
#endif

#endif /* __STAGE1_VALIDATION_TASK_H__ */
