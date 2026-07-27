/*
 * led_task.c - LED 灯效任务实现
 *
 * 以 50Hz 频率更新 LED 灯效。
 */

#include "led_task.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "led_service.h"

static const char *TAG = "led_task";

void led_task(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "led_task started");

    uint32_t last_update_ms = 0;
    uint32_t update_interval_ms = 20;  /* 50Hz 更新频率 */

    while (1) {
        uint32_t now_ms = esp_timer_get_time() / 1000;
        uint32_t dt_ms = now_ms - last_update_ms;
        last_update_ms = now_ms;

        /* 更新 LED 灯效 */
        led_service_update(dt_ms);

        vTaskDelay(pdMS_TO_TICKS(update_interval_ms));
    }
}
