/*
 * companion_app.c - Companion application implementation
 */

#include "companion_app.h"
#include "event_bus.h"
#include "event_def.h"
#include "led_service.h"
#include "lcd_service.h"
#include "servo_service.h"
#include "lamp_head_controller.h"
#include "tts_bridge.h"
#include "data_type.h"
#include "device_state.h"
#include "error_code.h"
#include "sensor_service.h"
#include "system_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "companion_app";

/* ===================== Private Data ===================== */
static bool s_initialized = false;
static bool s_running = false;

/* ===================== Internal Helpers ===================== */
/* 按 README 场景7：底部氛围呼吸灯（固定效果，不结合环境光），头部灯光关闭 */
static void companion_app_apply_lighting(void)
{
    led_service_set_effect(LED_EFFECT_BREATHING);
    lamp_head_led_off();
    ESP_LOGI(TAG, "Companion lighting: base breathing, head off");
}

/* ===================== Event Handlers ===================== */
static void companion_app_on_mode_changed(event_t *event, void *context)
{
    if (event == NULL || event->data == NULL) {
        return;
    }
    uint8_t *state_data = (uint8_t *)event->data;
    app_state_t new_state = (app_state_t)state_data[1];
    if (new_state != APP_STATE_COMPANION && s_running) {
        companion_app_stop();
    }
}

/* ===================== Public API ===================== */
esp_err_t companion_app_init(void)
{
    if (s_initialized) {
        return ERR_ALREADY_INITIALIZED;
    }

    esp_err_t ret = event_bus_subscribe(EV_APP_MODE_CHANGED, companion_app_on_mode_changed, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to EV_APP_MODE_CHANGED");
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Companion app initialized");
    return ESP_OK;
}

esp_err_t companion_app_start(void)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    if (s_running) {
        return ERR_BUSY;
    }

    s_running = true;

    /* 按 README 场景7：底部氛围呼吸灯，头部灯光关闭 */
    companion_app_apply_lighting();

    /* 大屏幕显示开心表情 */
    lamp_head_set_expression("happy");

    /* 小屏幕显示陪伴模式信息页（不显示表情） */
    lcd_service_mode_set(LCD_MODE_COMPANION);

    /* 先归位：使能舵机并回到 home 位置，机械臂保持静止，等待手势触发单步动作 */
    esp_err_t ret = servo_service_enable();
    if (ret == ESP_OK) {
        servo_service_go_home(1000);
        vTaskDelay(pdMS_TO_TICKS(1200));
    }

    event_bus_publish_simple(EV_APP_STATE_CHANGED);

    /* TTS播报：陪伴开启（短指令，云端映射完整文案） */
    tts_bridge_speak("陪伴开启");

    ESP_LOGI(TAG, "Companion app started");
    return ESP_OK;
}

esp_err_t companion_app_stop(void)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    if (!s_running) {
        return ERR_BUSY;
    }

    /* 陪伴模式不运行自动动作序列，退出时仅需归位（手势单步动作可能已使机械臂离开 home） */
    esp_err_t ret = servo_service_enable();
    if (ret == ESP_OK) {
        servo_service_go_home(1000);
        vTaskDelay(pdMS_TO_TICKS(1200));
    }

    /* 恢复头部表情为正常 */
    lamp_head_set_expression("neutral");

    /* Restore default lighting and LCD */
    led_service_turn_off();
    lcd_service_set_companion_overlay(false);
    lcd_service_page_switch_to(LCD_PAGE_EXPRESSION);
    lcd_service_expression_set(LCD_EXPRESSION_NORMAL);

    s_running = false;

    event_bus_publish_simple(EV_APP_STATE_CHANGED);
    ESP_LOGI(TAG, "Companion app stopped");
    return ESP_OK;
}