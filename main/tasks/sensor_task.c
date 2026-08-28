/*
 * sensor_task.c - 传感器采集任务实现
 *
 * 初始化雷达模块及分析器，处理 UART 事件与解析帧。
 * 雷达模块为必须依赖，初始化失败则退出本任务；分析器初始化失败仅告警继续运行。
 *
 * 当 CONFIG_PROJECT_RADAR_LD6002_ENABLE 开启时，雷达由独立的 radar_ld6002
 * 模块处理，本任务仅维持运行标志并负责环境光采集，避免与 radar_ld6002 争用同一 UART。
 */

#include "sensor_task.h"

#include "esp_err.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "event_bus.h"
#include "event_def.h"
#include "sensor_service.h"

#if !CONFIG_PROJECT_RADAR_LD6002_ENABLE
#include "radar_module.h"
#include "radar_frame_handler.h"
#include "hrv_analyzer.h"
#include "motion_analyzer.h"
#include "driver/uart.h"
#endif

static const char *TAG = "sensor_task";

/* 任务状态：运行标志 */
static bool s_radar_task_running = false;

void sensor_task(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "sensor_task started");

#if !CONFIG_PROJECT_RADAR_LD6002_ENABLE
    /* 雷达模块为必须依赖，初始化失败则退出本任务，不影响其他任务 */
    esp_err_t ret = radar_module_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Radar module init failed (%s), exiting sensor_task", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    /* HRV 分析器初始化非必须，失败仅告警继续循环 */
    ret = hrv_analyzer_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to init HRV analyzer: %s", esp_err_to_name(ret));
    }

    /* 运动分析器初始化非必须，失败仅告警继续循环 */
    ret = motion_analyzer_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to init motion analyzer: %s", esp_err_to_name(ret));
    }

    /* 初始化雷达帧处理器 */
    radar_frame_handler_init();

    /* 获取雷达事件队列 */
    QueueHandle_t radar_event_queue = radar_module_get_event_queue();
    QueueHandle_t frame_queue = radar_module_get_frame_queue();

    if (radar_event_queue == NULL || frame_queue == NULL) {
        ESP_LOGW(TAG, "Radar queues are NULL, exiting sensor_task");
        radar_module_deinit();
        vTaskDelete(NULL);
        return;
    }

    s_radar_task_running = true;
    ESP_LOGI(TAG, "Radar sensor task initialized");
#else
    ESP_LOGI(TAG, "Radar handled by radar_ld6002; sensor_task runs ambient-light only");
    s_radar_task_running = true;
#endif

#if !CONFIG_PROJECT_RADAR_LD6002_ENABLE
    uint32_t elapsed_ms = 0;
    const TickType_t start_tick = xTaskGetTickCount();
#endif

    while (1) {
#if !CONFIG_PROJECT_RADAR_LD6002_ENABLE
        elapsed_ms = pdTICKS_TO_MS(xTaskGetTickCount() - start_tick);

        /* 处理来自雷达驱动的 UART 事件 */
        uart_event_t uart_event = {0};
        BaseType_t has_event = xQueueReceive(radar_event_queue, &uart_event, pdMS_TO_TICKS(100));

        if (has_event == pdPASS) {
            esp_err_t evt_ret = radar_module_handle_uart_event(&uart_event);
            if (evt_ret != ESP_OK) {
                ESP_LOGW(TAG, "UART event handling failed: %s", esp_err_to_name(evt_ret));
            }
        }

        /* 处理已解析的帧 */
        tf_frame_t frame = {0};
        while (xQueueReceive(frame_queue, &frame, 0) == pdPASS) {
            radar_frame_handler_handle(&frame, elapsed_ms);
        }

        /* 检测雷达超时 */
        if (radar_module_is_timeout()) {
            static uint8_t s_radar_timeout_count = 0;
            if (s_radar_timeout_count < 3) {
                ESP_LOGW(TAG, "Radar data timeout");
            }
            s_radar_timeout_count++;
        }
#endif

        /* 读取环境光传感器（8 次过采样平均）并送入 PID 平滑流水线，
         * sensor_service 每攒满 10s（5×2s 块）发布一次平滑值
         * （EV_SENSOR_AMBIENT_LIGHT_CHANGED） */
        float lux = 0.0f;
        esp_err_t lret = sensor_service_get_light_lux(&lux);
        if (lret == ESP_OK) {
            sensor_service_feed_sample(lux);
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

bool sensor_task_is_running(void)
{
    return s_radar_task_running;
}
