/*
 * app_task.c - 应用层调度任务实现
 *
 * 监听应用状态机变化，执行各状态对应的 LED/LCD 切换与事件发布。
 */

#include "app_task.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_state.h"
#include "event_bus.h"
#include "event_def.h"
#include "led_service.h"
#include "lcd_service.h"
#include "led_module.h"

static const char *TAG = "app_task";

void app_task(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "app_task started");

    app_state_t current_state = APP_STATE_IDLE;
    app_state_t last_state = APP_STATE_INIT;

    while (1) {
        current_state = app_state_manager_get_state();

        /* 处理状态切换 */
        if (current_state != last_state) {
            ESP_LOGI(TAG, "App state changed: %d -> %d", last_state, current_state);
            last_state = current_state;

            /* 状态切换处理 */
            switch (current_state) {
            case APP_STATE_IDLE:
                /* 回到空闲态，关闭灯效 */
                led_service_turn_off();
                lcd_service_page_switch_to(LCD_PAGE_EXPRESSION);
                break;

            case APP_STATE_LIGHTING:
                /* 照明模式 - 简单 LED 控制 */
                led_service_set_effect(LED_EFFECT_STEADY);
                lcd_service_page_switch_to(LCD_PAGE_EXPRESSION);
                break;

            case APP_STATE_FOCUS:
                /* 专注模式 - 底部灯光关闭，灯头亮度由 focus_app 按光感档位控制 */
                led_service_turn_off();
                lcd_service_page_switch_to(LCD_PAGE_INFO);
                break;

            case APP_STATE_COMPANION:
                /* 陪伴模式 - LCD 显示模式信息页（不显示表情） */
                lcd_service_page_switch_to(LCD_PAGE_INFO);
                break;

            case APP_STATE_ARM_ACTION:
                /* 机械臂动作模式 */
                lcd_service_page_switch_to(LCD_PAGE_INFO);
                break;

            case APP_STATE_MUSIC_RHYTHM:
                /* 音乐节奏模式 */
                led_service_set_effect(LED_EFFECT_MUSIC_RHYTHM);
                lcd_service_page_switch_to(LCD_PAGE_INFO);
                break;

            case APP_STATE_GAME:
                /* 游戏模式 */
                lcd_service_page_switch_to(LCD_PAGE_INFO);
                break;

            case APP_STATE_VOICE:
                /* 语音控制模式 */
                lcd_service_page_switch_to(LCD_PAGE_INFO);
                break;

            case APP_STATE_EXERCISE_FOLLOW:
                /* 运动跟做模式 - 由 exercise_follow_app 处理 */
                led_service_set_mode(LED_MODE_COLOR);
                led_service_set_color(0, 180, 80);
                lcd_service_page_switch_to(LCD_PAGE_INFO);
                break;

            case APP_STATE_CUSTOM_RULE:
                /* 自定义规则模式 - 由 custom_rule_app 处理 */
                led_service_set_mode(LED_MODE_AMBIENT);
                led_service_set_color(100, 100, 255);
                lcd_service_page_switch_to(LCD_PAGE_INFO);
                break;

            case APP_STATE_SLEEP:
                /* 睡眠模式 - 低功耗 */
                lcd_service_sleep();
                led_service_turn_off();
                break;

            case APP_STATE_ERROR:
                /* 错误状态 */
                lcd_service_expression_set(LCD_EXPRESSION_SAD);
                led_service_show_system_state(LED_MODULE_SYSTEM_STATE_ERROR);
                break;

            default:
                break;
            }

            /* EV_APP_MODE_CHANGED 已由 app_state_manager_set_state() 正确发布
             * (含 old_state + new_state 两字节数据)，此处无需重复发布 */
        }

        /* 基于当前状态的应用层周期性处理 */
        switch (current_state) {
        case APP_STATE_FOCUS:
            /* 专注模式更新 - 可显示计时器、心率等 */
            break;

        case APP_STATE_MUSIC_RHYTHM:
            /* 音乐节奏 - LED 与音频同步 */
            break;

        default:
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
