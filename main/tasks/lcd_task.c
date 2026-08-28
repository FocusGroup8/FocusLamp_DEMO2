/*
 * lcd_task.c - LCD 显示任务实现
 *
 * 开启背光并显示表情页，随后以 20Hz 频率刷新 LCD 显示内容。
 */

#include "lcd_task.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lcd_driver.h"
#include "lcd_service.h"

static const char *TAG = "lcd_task";

void lcd_task(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "lcd_task started");

    /* 开启背光（取消开机 RGB 闪烁测试，直接进入表情页显示） */
    lcd_driver_set_backlight(255);
    lcd_fill_screen(LCD_COLOR_BLACK);

    /* 确保启动时显示表情页（page 0），强制触发页面回调与表情渲染 */
    lcd_service_page_switch_to(LCD_PAGE_EXPRESSION);
    lcd_service_expression_set(LCD_EXPRESSION_NORMAL);

    /* 由 lcd_service 驱动的周期性刷新（表情/信息页） */
    while (1) {
        lcd_service_update();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
