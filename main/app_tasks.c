/*
 * app_tasks.c - FreeRTOS 任务创建实现 for FocusLamp
 *
 * 仅负责任务表的登记与统一创建，各任务实现已拆分到 main/tasks/ 目录下的独立文件。
 * 单个板块任务初始化失败只会告警或自退出，不影响其他板块。
 */

#include <stdint.h>

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_tasks.h"

/* 各任务头文件 */
#include "servo_task.h"
#include "sensor_task.h"
#include "touch_task.h"
#include "led_task.h"
#include "lcd_task.h"
#include "app_task.h"
#include "watchdog_task.h"

#if CONFIG_PROJECT_RADAR_LD6002_ENABLE
#include "radar_ld6002.h"
#endif

static const char *TAG = "app_tasks";

/* ===================== 任务表 ===================== */

typedef struct {
    TaskFunction_t   function;
    const char      *name;
    uint32_t         stack_size;
    UBaseType_t      priority;
    BaseType_t       core_id;
    const char      *description;
} task_entry_t;

static const task_entry_t s_task_table[] = {
    {
        .function    = servo_task,
        .name        = "servo_task",
        .stack_size  = 3072,
        .priority    = 4,
        .core_id     = tskNO_AFFINITY,
        .description = "舵机控制任务",
    },
    {
        .function    = sensor_task,
        .name        = "sensor_task",
        .stack_size  = 4096,
        .priority    = 3,
        .core_id     = tskNO_AFFINITY,
        .description = "传感器采集任务",
    },
    {
        .function    = touch_task,
        .name        = "touch_task",
        .stack_size  = 8192,
        .priority    = 3,
        .core_id     = tskNO_AFFINITY,
        .description = "触摸检测任务",
    },
    {
        .function    = led_task,
        .name        = "led_task",
        .stack_size  = 4096,
        .priority    = 2,
        .core_id     = tskNO_AFFINITY,
        .description = "LED 灯效任务",
    },
    {
        .function    = lcd_task,
        .name        = "lcd_task",
        .stack_size  = 4096,
        .priority    = 5,
        .core_id     = tskNO_AFFINITY,
        .description = "LCD 显示任务",
    },
    {
        .function    = app_task,
        .name        = "app_task",
        .stack_size  = 4096,
        .priority    = 1,
        .core_id     = tskNO_AFFINITY,
        .description = "应用层调度任务",
    },
    {
        .function    = watchdog_task,
        .name        = "watchdog_task",
        .stack_size  = 2048,
        .priority    = 6,
        .core_id     = tskNO_AFFINITY,
        .description = "系统看门狗任务",
    },
};

static const int s_task_count = sizeof(s_task_table) / sizeof(s_task_table[0]);

/* ===================== 公共接口 ===================== */

void app_tasks_create(void)
{
    TaskHandle_t task_handle;

    ESP_LOGI(TAG, "Creating %d system tasks...", s_task_count);

    for (int i = 0; i < s_task_count; i++) {
        BaseType_t ret = xTaskCreatePinnedToCore(
            s_task_table[i].function,
            s_task_table[i].name,
            s_task_table[i].stack_size,
            NULL,
            s_task_table[i].priority,
            &task_handle,
            s_task_table[i].core_id
        );

        if (ret == pdPASS) {
            ESP_LOGI(TAG, "  [%d/%d] Created '%s' (prio=%d, stack=%d) - %s",
                     i + 1, s_task_count,
                     s_task_table[i].name,
                     s_task_table[i].priority,
                     s_task_table[i].stack_size,
                     s_task_table[i].description);
        } else {
            ESP_LOGE(TAG, "  [%d/%d] FAILED to create '%s'", i + 1, s_task_count, s_task_table[i].name);
        }
    }

    ESP_LOGI(TAG, "All tasks creation complete.");

#if CONFIG_PROJECT_RADAR_LD6002_ENABLE
    /* 自包含的 HLK-LD6002 雷达读取任务，独立于现有 radar_module 流水线，
     * 不影响其他功能。仅在 Kconfig 开启时启动。 */
    {
        esp_err_t rret = radar_ld6002_init();
        if (rret == ESP_OK) {
            rret = radar_ld6002_start();
        }
        if (rret != ESP_OK) {
            ESP_LOGW(TAG, "radar_ld6002 start failed: %s (continuing)",
                     esp_err_to_name(rret));
        } else {
            ESP_LOGI(TAG, "radar_ld6002 reader task started");
        }
    }
#endif
}

bool app_tasks_radar_is_running(void)
{
    return sensor_task_is_running();
}
