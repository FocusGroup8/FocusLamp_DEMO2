/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

#include "task_manager.h"
#include "task_manager_config.h"

#if (TASK_MANAGER_ENABLE == 1)

#include "esp_mcp_engine.h"
#include "esp_mcp_tool.h"
#include "esp_mcp_property.h"
#include "esp_mcp_data.h"

static const char *TAG = "TASK_MGR";

/*---------------------------------------------------------------
 * Internal data structures
 *-------------------------------------------------------------*/

typedef struct {
    bool in_use;                        /*!< Slot is occupied */
    task_info_t info;                   /*!< Task information */
    TimerHandle_t timer;                /*!< FreeRTOS timer for countdown */
} task_slot_t;

static task_slot_t s_tasks[TASK_MANAGER_MAX_TASKS];
static SemaphoreHandle_t s_mutex = NULL;
static bool s_initialized = false;

/*---------------------------------------------------------------
 * Internal: Timer callback (1-second period)
 *-------------------------------------------------------------*/
static void task_timer_callback(TimerHandle_t xTimer)
{
    int task_id = (int)(intptr_t)pvTimerGetTimerID(xTimer);

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    if (!s_tasks[task_id].in_use || s_tasks[task_id].info.state != TASK_STATE_RUNNING) {
        xSemaphoreGive(s_mutex);
        return;
    }

    s_tasks[task_id].info.remaining_seconds--;

    if (s_tasks[task_id].info.remaining_seconds <= 0) {
        s_tasks[task_id].info.remaining_seconds = 0;
        s_tasks[task_id].info.state = TASK_STATE_COMPLETED;

        /* Stop and delete the timer */
        xTimerStop(xTimer, 0);

        ESP_LOGI(TAG, "Task completed: \"%s\" (id=%d)", s_tasks[task_id].info.name, task_id);
    }

    xSemaphoreGive(s_mutex);
}

/*---------------------------------------------------------------
 * Internal: Find a free task slot
 *-------------------------------------------------------------*/
static int find_free_slot(void)
{
    for (int i = 0; i < TASK_MANAGER_MAX_TASKS; i++) {
        if (!s_tasks[i].in_use) {
            return i;
        }
    }
    return -1;
}

/*---------------------------------------------------------------
 * Internal: Validate task ID
 *-------------------------------------------------------------*/
static bool is_valid_task_id(int task_id)
{
    return (task_id >= 0 && task_id < TASK_MANAGER_MAX_TASKS && s_tasks[task_id].in_use);
}

/*---------------------------------------------------------------
 * Public API: Init / Deinit
 *-------------------------------------------------------------*/
esp_err_t task_manager_init(void)
{
    ESP_RETURN_ON_FALSE(!s_initialized, ESP_ERR_INVALID_STATE, TAG, "Already initialized");

    s_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_mutex, ESP_ERR_NO_MEM, TAG, "Failed to create mutex");

    memset(s_tasks, 0, sizeof(s_tasks));

    s_initialized = true;
    ESP_LOGI(TAG, "Task manager initialized (max_tasks=%d)", TASK_MANAGER_MAX_TASKS);

    return ESP_OK;
}

esp_err_t task_manager_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    /* Stop and delete all timers, clean up all tasks */
    for (int i = 0; i < TASK_MANAGER_MAX_TASKS; i++) {
        if (s_tasks[i].in_use) {
            if (s_tasks[i].timer) {
                xTimerStop(s_tasks[i].timer, portMAX_DELAY);
                xTimerDelete(s_tasks[i].timer, portMAX_DELAY);
                s_tasks[i].timer = NULL;
            }
            s_tasks[i].in_use = false;
        }
    }

    if (s_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "Task manager deinitialized");

    return ESP_OK;
}

/*---------------------------------------------------------------
 * Public API: Create task
 *-------------------------------------------------------------*/
int task_manager_create_task(const char *name, int type, int duration_minutes)
{
    ESP_RETURN_ON_FALSE(s_initialized, -1, TAG, "Not initialized");
    ESP_RETURN_ON_FALSE(name, -1, TAG, "Invalid name");
    ESP_RETURN_ON_FALSE(type >= TASK_TYPE_FOCUS && type <= TASK_TYPE_CUSTOM, -1, TAG, "Invalid type");
    ESP_RETURN_ON_FALSE(duration_minutes >= 1 && duration_minutes <= 480, -1, TAG, "Invalid duration");

    int task_id = -1;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire mutex");
        return -1;
    }

    task_id = find_free_slot();
    if (task_id < 0) {
        ESP_LOGW(TAG, "No free task slots (max=%d)", TASK_MANAGER_MAX_TASKS);
        xSemaphoreGive(s_mutex);
        return -1;
    }

    /* Create a 1-second periodic timer */
    s_tasks[task_id].timer = xTimerCreate(
        "task_timer",
        pdMS_TO_TICKS(1000),
        pdTRUE,  /* auto-reload */
        (void *)(intptr_t)task_id,
        task_timer_callback
    );
    if (!s_tasks[task_id].timer) {
        ESP_LOGE(TAG, "Failed to create timer for task");
        xSemaphoreGive(s_mutex);
        return -1;
    }

    s_tasks[task_id].in_use = true;
    s_tasks[task_id].info.id = task_id;
    strncpy(s_tasks[task_id].info.name, name, sizeof(s_tasks[task_id].info.name) - 1);
    s_tasks[task_id].info.name[sizeof(s_tasks[task_id].info.name) - 1] = '\0';
    s_tasks[task_id].info.type = type;
    s_tasks[task_id].info.duration_minutes = duration_minutes;
    s_tasks[task_id].info.remaining_seconds = duration_minutes * 60;
    s_tasks[task_id].info.state = TASK_STATE_IDLE;
    s_tasks[task_id].info.created_at = (int64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS / 1000);

    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Task created: \"%s\" (id=%d, type=%d, duration=%d min)",
             name, task_id, type, duration_minutes);

    return task_id;
}

/*---------------------------------------------------------------
 * Public API: Start task
 *-------------------------------------------------------------*/
esp_err_t task_manager_start_task(int task_id)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "Not initialized");
    ESP_RETURN_ON_FALSE(task_id >= 0 && task_id < TASK_MANAGER_MAX_TASKS, ESP_ERR_INVALID_ARG, TAG, "Invalid task id");

    esp_err_t ret = ESP_OK;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire mutex");
        return ESP_ERR_TIMEOUT;
    }

    if (!is_valid_task_id(task_id)) {
        ESP_LOGW(TAG, "Task %d not found", task_id);
        ret = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }
    if (s_tasks[task_id].info.state != TASK_STATE_IDLE) {
        ESP_LOGW(TAG, "Task %d not in IDLE state (state=%d)", task_id, s_tasks[task_id].info.state);
        ret = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }

    s_tasks[task_id].info.state = TASK_STATE_RUNNING;

    if (xTimerStart(s_tasks[task_id].timer, 0) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to start timer for task %d", task_id);
        s_tasks[task_id].info.state = TASK_STATE_IDLE;
        ret = ESP_FAIL;
        goto cleanup;
    }

    ESP_LOGI(TAG, "Task started: \"%s\" (id=%d)", s_tasks[task_id].info.name, task_id);

cleanup:
    xSemaphoreGive(s_mutex);
    return ret;
}

/*---------------------------------------------------------------
 * Public API: Pause task
 *-------------------------------------------------------------*/
esp_err_t task_manager_pause_task(int task_id)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "Not initialized");
    ESP_RETURN_ON_FALSE(task_id >= 0 && task_id < TASK_MANAGER_MAX_TASKS, ESP_ERR_INVALID_ARG, TAG, "Invalid task id");

    esp_err_t ret = ESP_OK;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire mutex");
        return ESP_ERR_TIMEOUT;
    }

    if (!is_valid_task_id(task_id)) {
        ESP_LOGW(TAG, "Task %d not found", task_id);
        ret = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }
    if (s_tasks[task_id].info.state != TASK_STATE_RUNNING) {
        ESP_LOGW(TAG, "Task %d not in RUNNING state", task_id);
        ret = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }

    s_tasks[task_id].info.state = TASK_STATE_PAUSED;
    xTimerStop(s_tasks[task_id].timer, 0);

    ESP_LOGI(TAG, "Task paused: \"%s\" (id=%d, remaining=%d sec)",
             s_tasks[task_id].info.name, task_id, s_tasks[task_id].info.remaining_seconds);

cleanup:
    xSemaphoreGive(s_mutex);
    return ret;
}

/*---------------------------------------------------------------
 * Public API: Resume task
 *-------------------------------------------------------------*/
esp_err_t task_manager_resume_task(int task_id)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "Not initialized");
    ESP_RETURN_ON_FALSE(task_id >= 0 && task_id < TASK_MANAGER_MAX_TASKS, ESP_ERR_INVALID_ARG, TAG, "Invalid task id");

    esp_err_t ret = ESP_OK;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire mutex");
        return ESP_ERR_TIMEOUT;
    }

    if (!is_valid_task_id(task_id)) {
        ESP_LOGW(TAG, "Task %d not found", task_id);
        ret = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }
    if (s_tasks[task_id].info.state != TASK_STATE_PAUSED) {
        ESP_LOGW(TAG, "Task %d not in PAUSED state", task_id);
        ret = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }

    s_tasks[task_id].info.state = TASK_STATE_RUNNING;

    if (xTimerStart(s_tasks[task_id].timer, 0) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to restart timer for task %d", task_id);
        s_tasks[task_id].info.state = TASK_STATE_PAUSED;
        ret = ESP_FAIL;
        goto cleanup;
    }

    ESP_LOGI(TAG, "Task resumed: \"%s\" (id=%d, remaining=%d sec)",
             s_tasks[task_id].info.name, task_id, s_tasks[task_id].info.remaining_seconds);

cleanup:
    xSemaphoreGive(s_mutex);
    return ret;
}

/*---------------------------------------------------------------
 * Public API: Stop task
 *-------------------------------------------------------------*/
esp_err_t task_manager_stop_task(int task_id)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "Not initialized");
    ESP_RETURN_ON_FALSE(task_id >= 0 && task_id < TASK_MANAGER_MAX_TASKS, ESP_ERR_INVALID_ARG, TAG, "Invalid task id");

    esp_err_t ret = ESP_OK;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire mutex");
        return ESP_ERR_TIMEOUT;
    }

    if (!is_valid_task_id(task_id)) {
        ESP_LOGW(TAG, "Task %d not found", task_id);
        ret = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    task_state_t prev_state = s_tasks[task_id].info.state;

    /* Stop the timer if it's active */
    if (s_tasks[task_id].timer) {
        xTimerStop(s_tasks[task_id].timer, 0);
        xTimerDelete(s_tasks[task_id].timer, portMAX_DELAY);
        s_tasks[task_id].timer = NULL;
    }

    s_tasks[task_id].info.state = TASK_STATE_CANCELLED;
    s_tasks[task_id].in_use = false;

    ESP_LOGI(TAG, "Task stopped: \"%s\" (id=%d, prev_state=%d)",
             s_tasks[task_id].info.name, task_id, prev_state);

cleanup:
    xSemaphoreGive(s_mutex);
    return ret;
}

/*---------------------------------------------------------------
 * Public API: Stop all tasks
 *-------------------------------------------------------------*/
esp_err_t task_manager_stop_all_tasks(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "Not initialized");

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire mutex");
        return ESP_ERR_TIMEOUT;
    }

    int stopped_count = 0;
    for (int i = 0; i < TASK_MANAGER_MAX_TASKS; i++) {
        if (s_tasks[i].in_use && (s_tasks[i].info.state == TASK_STATE_RUNNING ||
                                   s_tasks[i].info.state == TASK_STATE_PAUSED ||
                                   s_tasks[i].info.state == TASK_STATE_IDLE)) {
            if (s_tasks[i].timer) {
                xTimerStop(s_tasks[i].timer, 0);
                xTimerDelete(s_tasks[i].timer, portMAX_DELAY);
                s_tasks[i].timer = NULL;
            }
            s_tasks[i].info.state = TASK_STATE_CANCELLED;
            s_tasks[i].in_use = false;
            stopped_count++;
            ESP_LOGI(TAG, "Task stopped: \"%s\" (id=%d)", s_tasks[i].info.name, i);
        }
    }

    xSemaphoreGive(s_mutex);

    if (stopped_count == 0) {
        ESP_LOGW(TAG, "No running tasks to stop");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Stopped %d task(s)", stopped_count);
    return ESP_OK;
}

/*---------------------------------------------------------------
 * Public API: Get task info
 *-------------------------------------------------------------*/
esp_err_t task_manager_get_task(int task_id, task_info_t *info)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "Not initialized");
    ESP_RETURN_ON_FALSE(info, ESP_ERR_INVALID_ARG, TAG, "Invalid info pointer");
    ESP_RETURN_ON_FALSE(task_id >= 0 && task_id < TASK_MANAGER_MAX_TASKS, ESP_ERR_INVALID_ARG, TAG, "Invalid task id");

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire mutex");
        return ESP_ERR_TIMEOUT;
    }

    if (!is_valid_task_id(task_id)) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    memcpy(info, &s_tasks[task_id].info, sizeof(task_info_t));

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

/*---------------------------------------------------------------
 * Public API: Get running count
 *-------------------------------------------------------------*/
int task_manager_get_running_count(void)
{
    if (!s_initialized) {
        return 0;
    }

    int count = 0;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        for (int i = 0; i < TASK_MANAGER_MAX_TASKS; i++) {
            if (s_tasks[i].in_use && s_tasks[i].info.state == TASK_STATE_RUNNING) {
                count++;
            }
        }
        xSemaphoreGive(s_mutex);
    }

    return count;
}

/*---------------------------------------------------------------
 * Public API: Has running tasks
 *-------------------------------------------------------------*/
bool task_manager_has_running_tasks(void)
{
    return task_manager_get_running_count() > 0;
}

/*---------------------------------------------------------------
 * MCP tool callbacks
 *-------------------------------------------------------------*/

/**
 * @brief MCP tool: self.task.create
 */
static esp_mcp_value_t mcp_tool_task_create(const esp_mcp_property_list_t *properties)
{
    const char *name = esp_mcp_property_list_get_property_string(properties, "name");
    int type = esp_mcp_property_list_get_property_int(properties, "type");
    int duration_minutes = esp_mcp_property_list_get_property_int(properties, "duration_minutes");

    ESP_LOGI(TAG, "[MCP] self.task.create: name=\"%s\" type=%d duration=%d min",
             name ? name : "null", type, duration_minutes);

    int task_id = task_manager_create_task(name, type, duration_minutes);

    bool success = (task_id >= 0);
    return esp_mcp_value_create_bool(success);
}

/**
 * @brief MCP tool: self.task.stop
 */
static esp_mcp_value_t mcp_tool_task_stop(const esp_mcp_property_list_t *properties)
{
    int task_id = esp_mcp_property_list_get_property_int(properties, "task_id");

    ESP_LOGI(TAG, "[MCP] self.task.stop: task_id=%d", task_id);

    esp_err_t ret = task_manager_stop_task(task_id);

    return esp_mcp_value_create_bool(ret == ESP_OK);
}

/**
 * @brief MCP tool: self.task.stop_all
 */
static esp_mcp_value_t mcp_tool_task_stop_all(const esp_mcp_property_list_t *properties)
{
    ESP_LOGI(TAG, "[MCP] self.task.stop_all");

    esp_err_t ret = task_manager_stop_all_tasks();

    return esp_mcp_value_create_bool(ret == ESP_OK);
}

/**
 * @brief MCP tool: self.task.list
 *
 * Returns a JSON string with all task information.
 */
static esp_mcp_value_t mcp_tool_task_list(const esp_mcp_property_list_t *properties)
{
    ESP_LOGI(TAG, "[MCP] self.task.list");

    static char json_buf[512];
    int offset = 0;

    offset += snprintf(json_buf + offset, sizeof(json_buf) - offset, "[");

    bool first = true;
    for (int i = 0; i < TASK_MANAGER_MAX_TASKS; i++) {
        if (!s_tasks[i].in_use) {
            continue;
        }

        if (!first) {
            offset += snprintf(json_buf + offset, sizeof(json_buf) - offset, ",");
        }
        first = false;

        const char *state_str;
        switch (s_tasks[i].info.state) {
        case TASK_STATE_IDLE:      state_str = "idle"; break;
        case TASK_STATE_RUNNING:   state_str = "running"; break;
        case TASK_STATE_PAUSED:    state_str = "paused"; break;
        case TASK_STATE_COMPLETED: state_str = "completed"; break;
        case TASK_STATE_CANCELLED: state_str = "cancelled"; break;
        default:                   state_str = "unknown"; break;
        }

        const char *type_str;
        switch (s_tasks[i].info.type) {
        case TASK_TYPE_FOCUS:  type_str = "focus"; break;
        case TASK_TYPE_STUDY:  type_str = "study"; break;
        case TASK_TYPE_WORK:   type_str = "work"; break;
        case TASK_TYPE_CUSTOM: type_str = "custom"; break;
        default:               type_str = "unknown"; break;
        }

        int remaining_min = s_tasks[i].info.remaining_seconds / 60;
        int remaining_sec = s_tasks[i].info.remaining_seconds % 60;

        offset += snprintf(json_buf + offset, sizeof(json_buf) - offset,
                           "{\"id\":%d,\"name\":\"%s\",\"type\":\"%s\",\"duration_min\":%d,\"remaining\":\"%d:%02d\",\"state\":\"%s\"}",
                           s_tasks[i].info.id,
                           s_tasks[i].info.name,
                           type_str,
                           s_tasks[i].info.duration_minutes,
                           remaining_min, remaining_sec,
                           state_str);

        /* Prevent buffer overflow */
        if (offset >= (int)sizeof(json_buf) - 2) {
            break;
        }
    }

    offset += snprintf(json_buf + offset, sizeof(json_buf) - offset, "]");

    return esp_mcp_value_create_string(json_buf);
}

/*---------------------------------------------------------------
 * Public API: Register MCP tools
 *-------------------------------------------------------------*/
esp_err_t task_manager_register_mcp_tools(esp_mcp_t *mcp)
{
    ESP_RETURN_ON_FALSE(mcp, ESP_ERR_INVALID_ARG, TAG, "Invalid MCP engine");

    /* self.task.create */
    esp_mcp_tool_t *create_tool = esp_mcp_tool_create(
        "self.task.create",
        "创建一个新任务（计时器），支持专注、学习、工作和自定义类型",
        mcp_tool_task_create
    );
    if (!create_tool) {
        ESP_LOGE(TAG, "Failed to create self.task.create tool");
        return ESP_ERR_NO_MEM;
    }
    esp_mcp_property_t *name_prop = esp_mcp_property_create("name", ESP_MCP_PROPERTY_TYPE_STRING);
    esp_mcp_tool_add_property(create_tool, name_prop);
    esp_mcp_property_t *type_prop = esp_mcp_property_create_with_range("type", 0, 3);
    esp_mcp_tool_add_property(create_tool, type_prop);
    esp_mcp_property_t *dur_prop = esp_mcp_property_create_with_range("duration_minutes", 1, 480);
    esp_mcp_tool_add_property(create_tool, dur_prop);
    esp_mcp_add_tool(mcp, create_tool);

    /* self.task.stop */
    esp_mcp_tool_t *stop_tool = esp_mcp_tool_create(
        "self.task.stop",
        "停止一个任务",
        mcp_tool_task_stop
    );
    if (!stop_tool) {
        ESP_LOGE(TAG, "Failed to create self.task.stop tool");
        return ESP_ERR_NO_MEM;
    }
    esp_mcp_property_t *stop_id_prop = esp_mcp_property_create_with_range("task_id", 0, TASK_MANAGER_MAX_TASKS - 1);
    esp_mcp_tool_add_property(stop_tool, stop_id_prop);
    esp_mcp_add_tool(mcp, stop_tool);

    /* self.task.stop_all */
    esp_mcp_tool_t *stop_all_tool = esp_mcp_tool_create(
        "self.task.stop_all",
        "停止所有任务",
        mcp_tool_task_stop_all
    );
    if (!stop_all_tool) {
        ESP_LOGE(TAG, "Failed to create self.task.stop_all tool");
        return ESP_ERR_NO_MEM;
    }
    esp_mcp_add_tool(mcp, stop_all_tool);

    /* self.task.list */
    esp_mcp_tool_t *list_tool = esp_mcp_tool_create(
        "self.task.list",
        "列出所有任务及其状态",
        mcp_tool_task_list
    );
    if (!list_tool) {
        ESP_LOGE(TAG, "Failed to create self.task.list tool");
        return ESP_ERR_NO_MEM;
    }
    esp_mcp_add_tool(mcp, list_tool);

    ESP_LOGI(TAG, "Task MCP tools registered (self.task.create, self.task.stop, self.task.stop_all, self.task.list)");
    return ESP_OK;
}

#else /* TASK_MANAGER_ENABLE == 0 */

/* Stub implementations when component is disabled.
 * Static inline stubs are provided in task_manager.h,
 * so no separate definitions are needed here.
 */

#endif /* TASK_MANAGER_ENABLE */
