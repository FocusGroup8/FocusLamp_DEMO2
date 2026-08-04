/*
 * touch_task.c - 触摸检测任务实现
 *
 * 启动触摸服务后以 100Hz 频率轮询处理触摸事件。
 */

#include "touch_task.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "touch_service.h"

static const char *TAG = "touch_task";

void touch_task(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "touch_task started");

    touch_service_start();

    while (1) {
        touch_service_process();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
