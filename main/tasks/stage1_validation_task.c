/*
 * stage1_validation_task.c - Stage1 硬件验证任务实现
 *
 * 初始化雷达模块，进入验证模式输出解析的帧数据。
 */

#include "stage1_validation_task.h"

#include <string.h>
#include <inttypes.h>

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "driver/uart.h"

#include "radar_module.h"
#include "system_config.h"

static const char *TAG = "stage1_validation";

static TaskHandle_t s_validation_task_handle  = NULL;
static bool         s_validation_task_running = false;

#define VALIDATION_TASK_NAME        "validation_task"
#define VALIDATION_TASK_STACK_SIZE  4096
#define VALIDATION_TASK_PRIORITY    2
#define VALIDATION_TASK_PERIOD_MS   50

static void log_frame(const struct tf_frame *frame)
{
    if (frame == NULL) return;

    ESP_LOGI(TAG, "Frame type=0x%04X, id=%u, len=%u",
             frame->message_type, frame->frame_id, frame->data_length);
}

static void validation_task_entry(void *arg)
{
    (void)arg;

    esp_err_t ret = radar_module_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "radar_module_init failed: %s", esp_err_to_name(ret));
        s_validation_task_running = false;
        s_validation_task_handle  = NULL;
        vTaskDelete(NULL);
        return;
    }

    QueueHandle_t event_queue = radar_module_get_event_queue();
    if (event_queue == NULL) {
        ESP_LOGE(TAG, "Event queue is NULL");
        (void)radar_module_deinit();
        s_validation_task_running = false;
        s_validation_task_handle  = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Validation task started");

    while (true) {
        uart_event_t uart_evt = {0};
        if (xQueueReceive(event_queue, &uart_evt,
                          pdMS_TO_TICKS(VALIDATION_TASK_PERIOD_MS)) == pdPASS) {
            ret = radar_module_handle_uart_event(&uart_evt);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "handle_uart_event: %s", esp_err_to_name(ret));
            }
        }

        radar_module_snapshot_t snap = {0};
        if (radar_module_get_snapshot(&snap) == ESP_OK && snap.has_new_frame) {
            log_frame(&snap.last_frame);
            (void)radar_module_clear_new_frame_flag();
        }
    }
}

esp_err_t stage1_validation_task_start(void)
{
    if (s_validation_task_running) {
        ESP_LOGW(TAG, "Already running");
        return ESP_OK;
    }

    BaseType_t result = xTaskCreate(validation_task_entry, VALIDATION_TASK_NAME,
                                    VALIDATION_TASK_STACK_SIZE, NULL,
                                    VALIDATION_TASK_PRIORITY, &s_validation_task_handle);
    if (result != pdPASS) {
        s_validation_task_handle = NULL;
        return ESP_FAIL;
    }

    s_validation_task_running = true;
    ESP_LOGI(TAG, "Validation task started");
    return ESP_OK;
}

esp_err_t stage1_validation_task_stop(void)
{
    if (!s_validation_task_running || s_validation_task_handle == NULL) {
        return ESP_OK;
    }

    vTaskDelete(s_validation_task_handle);
    s_validation_task_handle  = NULL;
    s_validation_task_running = false;

    (void)radar_module_deinit();
    ESP_LOGI(TAG, "Validation task stopped");
    return ESP_OK;
}

bool stage1_validation_task_is_running(void)
{
    return s_validation_task_running;
}
