/*
 * app_tasks.h - FreeRTOS task creation interface for FocusLamp
 */

#pragma once
#ifndef __APP_TASKS_H__
#define __APP_TASKS_H__

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create all system FreeRTOS tasks.
 *        Tasks are created using xTaskCreatePinnedToCore.
 *        See app_tasks.c for the full task table.
 */
void app_tasks_create(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_TASKS_H__ */