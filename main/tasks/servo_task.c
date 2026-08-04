/*
 * servo_task.c - 舵机控制任务实现
 *
 * 使能舵机服务后以 20Hz 频率更新舵机状态，处理录制与回放流程。
 */

#include "servo_task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "servo_service.h"

static const char *TAG = "servo_task";

void servo_task(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "servo_task started");

    /* 初始化舵机控制模块，失败仅告警不影响其他任务 */
    esp_err_t ret = servo_service_enable();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to enable servos: %s", esp_err_to_name(ret));
    }

    uint32_t update_interval_ms = 50;  /* 20Hz 更新频率 */
    servo_service_status_t status;

    while (1) {
        /* 获取当前舵机服务状态 */
        status = servo_service_get_status();

        /* 处理录制状态 */
        if (status.current_state == SERVO_SERVICE_STATE_RECORDING) {
            /* 录制过程中采样舵机帧 */
            servo_service_frame_t frame;
            /* 从 servo_driver 填充帧数据 */
            for (int i = 0; i < 4; i++) {
                frame.lx_pos[i] = (int16_t)servo_service_get_position((uint8_t)i);
            }
            frame.em3_pos = (int16_t)servo_service_get_position(4);
            frame.time_ms = esp_timer_get_time() / 1000;

            /* 帧由 servo_service 内部处理 */
        }

        /* 处理回放状态 */
        if (status.current_state == SERVO_SERVICE_STATE_PLAYING) {
            /* 回放帧由 servo_service 内部处理 */
        }

        vTaskDelay(pdMS_TO_TICKS(update_interval_ms));
    }
}
