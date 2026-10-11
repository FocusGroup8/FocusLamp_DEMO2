/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "task_manager_config.h"

#if (TASK_MANAGER_ENABLE == 1)
/* Forward declaration – reduces coupling; esp_mcp_engine.h is included
   only in the .c file where the full definition is needed. */
typedef struct esp_mcp_s esp_mcp_t;
#else
/* Forward declaration for opaque MCP handle when component is disabled */
typedef struct esp_mcp_s esp_mcp_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Task states
 */
typedef enum {
    TASK_STATE_IDLE,        /*!< Task created but not started */
    TASK_STATE_RUNNING,     /*!< Task is running (countdown active) */
    TASK_STATE_PAUSED,      /*!< Task is paused */
    TASK_STATE_COMPLETED,   /*!< Task completed (countdown reached 0) */
    TASK_STATE_CANCELLED,   /*!< Task was cancelled by user */
} task_state_t;

/**
 * @brief  Task types
 */
typedef enum {
    TASK_TYPE_FOCUS,        /*!< Focus / concentration task */
    TASK_TYPE_STUDY,        /*!< Study task */
    TASK_TYPE_WORK,         /*!< Work task */
    TASK_TYPE_CUSTOM,       /*!< Custom task */
} task_type_t;

/**
 * @brief  Task information structure
 */
typedef struct {
    int id;                             /*!< Task ID (index in task array) */
    char name[32];                      /*!< Task name */
    int type;                           /*!< Task type (task_type_t) */
    int duration_minutes;               /*!< Total duration in minutes */
    int remaining_seconds;              /*!< Remaining time in seconds */
    task_state_t state;                 /*!< Current task state */
    int64_t created_at;                 /*!< Creation timestamp (seconds since boot) */
} task_info_t;

/**
 * @brief  Initialize task manager
 *
 * Allocates internal resources and initializes the task array.
 *
 * @return ESP_OK on success
 */
esp_err_t task_manager_init(void);

/**
 * @brief  Deinitialize task manager
 *
 * Stops all running tasks and releases resources.
 *
 * @return ESP_OK on success
 */
esp_err_t task_manager_deinit(void);

/**
 * @brief  Create a new task
 *
 * @param name              Task name (will be copied, max 31 chars)
 * @param type              Task type (task_type_t)
 * @param duration_minutes  Duration in minutes (1-480)
 * @return Task ID on success, -1 on error
 */
int task_manager_create_task(const char *name, int type, int duration_minutes);

/**
 * @brief  Start a task (begin countdown)
 *
 * @param task_id  Task ID returned by task_manager_create_task
 * @return ESP_OK on success
 */
esp_err_t task_manager_start_task(int task_id);

/**
 * @brief  Pause a running task
 *
 * @param task_id  Task ID
 * @return ESP_OK on success
 */
esp_err_t task_manager_pause_task(int task_id);

/**
 * @brief  Resume a paused task
 *
 * @param task_id  Task ID
 * @return ESP_OK on success
 */
esp_err_t task_manager_resume_task(int task_id);

/**
 * @brief  Stop/Cancel a task
 *
 * @param task_id  Task ID
 * @return ESP_OK on success
 */
esp_err_t task_manager_stop_task(int task_id);

/**
 * @brief  Stop all running tasks
 *
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no tasks are running
 */
esp_err_t task_manager_stop_all_tasks(void);

/**
 * @brief  Get task information
 *
 * @param task_id  Task ID
 * @param info     Pointer to task_info_t to fill
 * @return ESP_OK on success
 */
esp_err_t task_manager_get_task(int task_id, task_info_t *info);

/**
 * @brief  Get count of currently running tasks
 *
 * @return Number of running tasks
 */
int task_manager_get_running_count(void);

/**
 * @brief  Check if any tasks are currently running
 *
 * @return true if at least one task is running
 */
bool task_manager_has_running_tasks(void);

/**
 * @brief  Register MCP tools for task control
 *
 * Registers self.task.create, self.task.stop, self.task.stop_all,
 * and self.task.list tools on the given MCP engine.
 * The MCP engine handle is typically obtained from xiaozhi_manager_get_mcp_engine().
 *
 * @param mcp  MCP engine handle
 * @return ESP_OK on success
 */
esp_err_t task_manager_register_mcp_tools(esp_mcp_t *mcp);

/* Stub implementations when component is disabled */
#if (TASK_MANAGER_ENABLE == 0)

static inline esp_err_t task_manager_init(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t task_manager_deinit(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline int task_manager_create_task(const char *name, int type, int duration_minutes)
{
    (void)name; (void)type; (void)duration_minutes;
    return -1;
}
static inline esp_err_t task_manager_start_task(int task_id) { (void)task_id; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t task_manager_pause_task(int task_id) { (void)task_id; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t task_manager_resume_task(int task_id) { (void)task_id; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t task_manager_stop_task(int task_id) { (void)task_id; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t task_manager_stop_all_tasks(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t task_manager_get_task(int task_id, task_info_t *info) { (void)task_id; (void)info; return ESP_ERR_NOT_SUPPORTED; }
static inline int task_manager_get_running_count(void) { return 0; }
static inline bool task_manager_has_running_tasks(void) { return false; }
static inline esp_err_t task_manager_register_mcp_tools(esp_mcp_t *mcp) { (void)mcp; return ESP_ERR_NOT_SUPPORTED; }

#endif /* TASK_MANAGER_ENABLE == 0 */

#ifdef __cplusplus
}
#endif
