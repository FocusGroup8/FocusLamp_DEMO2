#include "lcd_status_ui.h"
#include "lcd_driver.h"
#include "controller_modes.h"
#include "radar_ld6002.h"
#include "esp_timer.h"
#include <math.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

static const char *TAG = "LCD_UI";
static bool s_started;

static bool display_changed(const controller_modes_status_t *a,
                            const controller_modes_status_t *b)
{
    return a->mode != b->mode || a->remaining_sec != b->remaining_sec ||
           a->paused != b->paused || a->completed != b->completed;
}

static void status_task(void *arg)
{
    (void)arg;
    controller_modes_status_t previous = {0};
    bool first = true;
    int previous_heart = -2;
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        controller_modes_status_t status;
        esp_err_t ret = controller_modes_get_status(&status);
        if (ret != ESP_OK) break;
        radar_ld6002_state_t radar = {0};
        esp_err_t radar_ret = radar_ld6002_get_state(&radar);
        int64_t now_ms = esp_timer_get_time() / 1000;
        int heart = -1;
        if (radar_ret == ESP_OK && radar.has_heart && radar.has_presence && radar.person_present &&
            radar.heart_updated_ms > 0 && now_ms >= radar.heart_updated_ms &&
            now_ms - radar.heart_updated_ms <= 10000 && radar.presence_updated_ms > 0 &&
            now_ms >= radar.presence_updated_ms && now_ms - radar.presence_updated_ms <= 3000 &&
            isfinite(radar.heart_rate) && radar.heart_rate > 0 && radar.heart_rate < 1000) {
            heart = (int)(radar.heart_rate + 0.5f);
        }
        if (first || display_changed(&status, &previous) || heart != previous_heart) {
            const char *name = status.mode == APP_STATE_FOCUS ? "FOCUS" :
                               status.mode == APP_STATE_COMPANION ? "COMPANION" : "IDLE";
            const char *state = status.completed ? "COMPLETE" :
                                status.mode == APP_STATE_FOCUS ? (status.paused ? "PAUSED" : "RUNNING") :
                                status.mode == APP_STATE_COMPANION ? "ACTIVE" : "READY";
            char time_text[16] = "--:--";
            if (status.mode == APP_STATE_FOCUS || status.completed) {
                snprintf(time_text, sizeof(time_text), "%02u:%02u",
                         (unsigned)(status.remaining_sec / 60),
                         (unsigned)(status.remaining_sec % 60));
            }
            lcd_fill_rect_buffer(0, 0, LCD_WIDTH, LCD_HEIGHT, LCD_COLOR_BLACK);
            lcd_draw_string_buffer(0, 0, name, LCD_COLOR_CYAN, LCD_COLOR_BLACK, 1);
            char heart_text[20] = "HR:-- bpm";
            if (heart >= 0) snprintf(heart_text, sizeof(heart_text), "HR:%d bpm", heart);
            lcd_draw_string_buffer(0, 12, heart_text, LCD_COLOR_GREEN, LCD_COLOR_BLACK, 1);
            lcd_draw_string_buffer(0, 24, time_text, LCD_COLOR_WHITE, LCD_COLOR_BLACK, 2);
            lcd_draw_string_buffer(0, 48, state, LCD_COLOR_YELLOW, LCD_COLOR_BLACK, 1);
            ret = lcd_flush_buffer();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "LCD refresh stopped: %s (other modules continue)", esp_err_to_name(ret));
                break;
            }
            previous = status;
            previous_heart = heart;
            first = false;
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(1000));
    }
    /* No logical-frame writer remains. A timed-out DMA buffer is retained. */
    esp_err_t ret = lcd_driver_deinit();
    if (ret != ESP_OK) ESP_LOGW(TAG, "LCD cleanup deferred: %s", esp_err_to_name(ret));
    vTaskDelete(NULL);
}

esp_err_t lcd_status_ui_start(void)
{
    if (s_started) return ESP_OK;
    controller_modes_status_t status;
    esp_err_t ret = controller_modes_get_status(&status);
    if (ret != ESP_OK) return ret;
    if (xTaskCreate(status_task, "lcd_status", LCD_TASK_STACK_SIZE, NULL,
                    LCD_TASK_PRIORITY, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    s_started = true;
    ESP_LOGI(TAG, "LCD status UI started: 1 s snapshot, no independent countdown");
    return ESP_OK;
}
