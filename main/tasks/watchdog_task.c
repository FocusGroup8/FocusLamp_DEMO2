/*
 * watchdog_task.c - 系统看门狗任务实现
 *
 * 周期性检测系统堆内存，低于阈值时发布系统错误事件。
 */

#include "watchdog_task.h"

#include "esp_log.h"
#include "esp_system.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "event_bus.h"
#include "event_def.h"

static const char *TAG = "watchdog_task";

void watchdog_task(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "watchdog_task started");

    uint32_t check_interval_ms = 5000;
    uint32_t heap_warning_threshold = 10000;  /* 10KB */

    while (1) {
        uint32_t free_heap = esp_get_free_heap_size();

        /* 内存告警 */
        if (free_heap < heap_warning_threshold) {
            ESP_LOGW(TAG, "Low heap: %lu bytes", free_heap);

            event_t ev = {
                .type = EV_SYS_ERROR,
                .data = &free_heap,
                .data_size = sizeof(free_heap),
                .timestamp = event_bus_get_timestamp()
            };
            event_bus_publish(&ev);
        }

        vTaskDelay(pdMS_TO_TICKS(check_interval_ms));
    }
}
