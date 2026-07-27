/*
 * audio_task.c - 音频处理任务实现
 *
 * 周期性查询音频服务状态，空闲时触发下一曲或通知完成。
 */

#include "audio_task.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_service.h"

static const char *TAG = "audio_task";

void audio_task(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "audio_task started");

    while (1) {
        /* 获取当前音频状态 */
        audio_service_state_t state = audio_service_get_state();
        if (state == AUDIO_SERVICE_STATE_IDLE) {
            /* 可触发下一曲或通知完成 */
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
