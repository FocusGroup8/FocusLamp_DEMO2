/*
 * radar_data_task.c - 雷达数据采集任务实现
 *
 * 初始化雷达模块，轮询 UART 事件，解码帧数据并分发给 frame_handler。
 */

#include "radar_data_task.h"

#include <inttypes.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "driver/uart.h"

#include "radar_module.h"
#include "radar_frame_handler.h"
#include "system_config.h"

static const char *TAG = "radar_data_task";

static TaskHandle_t s_radar_data_task_handle  = NULL;
static bool         s_radar_data_task_running = false;

/* project_config.h also defines these; keep the local (finer-grained) values
 * without triggering -Wmacro-redefined. */
#ifndef RADAR_TASK_PERIOD_MS
#define RADAR_TASK_PERIOD_MS    20
#endif
#ifndef RADAR_DATA_TIMEOUT_MS
#define RADAR_DATA_TIMEOUT_MS   15000
#endif
#ifndef RADAR_TASK_STACK_SIZE
#define RADAR_TASK_STACK_SIZE   TASK_STACK_RADAR
#endif
#ifndef RADAR_TASK_PRIORITY
#define RADAR_TASK_PRIORITY     PRIORITY_RADAR
#endif
#define RADAR_TASK_NAME         "radar_data_task"

static void radar_data_task_cleanup(void)
{
    (void)radar_module_deinit();
}

static void radar_data_task_entry(void *arg)
{
    (void)arg;

    esp_err_t ret = radar_module_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "radar_module_init failed: %s", esp_err_to_name(ret));
        s_radar_data_task_running = false;
        s_radar_data_task_handle  = NULL;
        vTaskDelete(NULL);
        return;
    }

    radar_frame_handler_init();

    QueueHandle_t radar_event_queue = radar_module_get_event_queue();
    if (radar_event_queue == NULL) {
        ESP_LOGE(TAG, "radar_module_get_event_queue returned NULL");
        radar_data_task_cleanup();
        s_radar_data_task_running = false;
        s_radar_data_task_handle  = NULL;
        vTaskDelete(NULL);
        return;
    }

    QueueHandle_t frame_queue = radar_module_get_frame_queue();
    if (frame_queue == NULL) {
        ESP_LOGE(TAG, "radar_module_get_frame_queue returned NULL");
        radar_data_task_cleanup();
        s_radar_data_task_running = false;
        s_radar_data_task_handle  = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Radar data task started");

    const TickType_t start_tick = xTaskGetTickCount();

    while (true) {
        const uint32_t elapsed_ms = pdTICKS_TO_MS(xTaskGetTickCount() - start_tick);

        /* 处理 UART 事件 */
        uart_event_t uart_event = {0};
        if (xQueueReceive(radar_event_queue, &uart_event,
                          pdMS_TO_TICKS(RADAR_TASK_PERIOD_MS)) == pdPASS) {
            ret = radar_module_handle_uart_event(&uart_event);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "radar_module_handle_uart_event: %s", esp_err_to_name(ret));
            }
        }

        /* 处理所有已解析的帧 */
        struct tf_frame frame = {0};
        while (xQueueReceive(frame_queue, &frame, 0) == pdPASS) {
            radar_frame_handler_handle(&frame, elapsed_ms);
        }

        /* 空闲超时检测 */
        int64_t time_since_last_ms = radar_module_get_time_since_last_receive_ms();
        if (time_since_last_ms > RADAR_DATA_TIMEOUT_MS) {
            ESP_LOGW(TAG, "No radar data for %" PRId64 " ms (threshold: %d ms). Stopping.",
                     time_since_last_ms, RADAR_DATA_TIMEOUT_MS);
            break;
        }
    }

    ESP_LOGI(TAG, "Radar data task exiting");
    radar_data_task_cleanup();
    s_radar_data_task_running = false;
    s_radar_data_task_handle  = NULL;
    vTaskDelete(NULL);
}

esp_err_t radar_data_task_start(void)
{
    if (s_radar_data_task_running) {
        ESP_LOGW(TAG, "Already running");
        return ESP_OK;
    }

    BaseType_t result = xTaskCreate(radar_data_task_entry, RADAR_TASK_NAME,
                                    RADAR_TASK_STACK_SIZE, NULL,
                                    RADAR_TASK_PRIORITY, &s_radar_data_task_handle);
    if (result != pdPASS) {
        s_radar_data_task_handle = NULL;
        return ESP_FAIL;
    }

    s_radar_data_task_running = true;
    ESP_LOGI(TAG, "Task started");
    return ESP_OK;
}

esp_err_t radar_data_task_stop(void)
{
    if (!s_radar_data_task_running || s_radar_data_task_handle == NULL) {
        return ESP_OK;
    }

    vTaskDelete(s_radar_data_task_handle);
    s_radar_data_task_handle  = NULL;
    s_radar_data_task_running = false;
    ESP_LOGI(TAG, "Task stopped");
    return ESP_OK;
}

bool radar_data_task_is_running(void)
{
    return s_radar_data_task_running;
}
