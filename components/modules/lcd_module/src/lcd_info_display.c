#include "lcd_info_display.h"

#include "esp_log.h"

#include "lcd_config.h"
#include "lcd_driver.h"
#include "string.h"

#if (RADAR_ENABLE_LCD_DISPLAY == 1)

static const char* TAG = "LCD_INFO_DISPLAY";

#define LCD_INFO_TITLE_Y 2
#define LCD_INFO_TITLE_SIZE 1

#define LCD_INFO_TIMER_Y 18
#define LCD_INFO_TIMER_SIZE 2

typedef struct
{
    char               task_title[LCD_INFO_TITLE_MAX_LEN];
    uint32_t           timer_seconds;
    bool               timer_running;
    bool               initialized;
} lcd_info_state_t;

static lcd_info_state_t s_info_state = {
    .task_title    = "Idle",
    .timer_seconds = 0,
    .timer_running = false,
    .initialized   = false,
};

static void format_timer(uint32_t seconds, char* buffer, size_t buffer_size)
{
    uint32_t minutes = seconds / 60;
    uint32_t secs    = seconds % 60;
    snprintf(buffer, buffer_size, "%02lu:%02lu", minutes, secs);
}

esp_err_t lcd_info_init(void)
{
    if (s_info_state.initialized)
    {
        ESP_LOGW(TAG, "Info display already initialized");
        return ESP_OK;
    }

    strncpy(s_info_state.task_title, "Idle", LCD_INFO_TITLE_MAX_LEN - 1);
    s_info_state.task_title[LCD_INFO_TITLE_MAX_LEN - 1] = '\0';
    s_info_state.timer_seconds                          = 0;
    s_info_state.timer_running                          = false;
    s_info_state.initialized                            = true;

    ESP_LOGI(TAG, "Info display initialized");
    return ESP_OK;
}

esp_err_t lcd_info_deinit(void)
{
    if (!s_info_state.initialized)
    {
        ESP_LOGW(TAG, "Info display not initialized");
        return ESP_OK;
    }

    s_info_state.initialized = false;
    ESP_LOGI(TAG, "Info display deinitialized");
    return ESP_OK;
}

esp_err_t lcd_info_set_title(const char* title)
{
    if (!s_info_state.initialized)
    {
        ESP_LOGW(TAG, "Info display not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (title == NULL)
    {
        ESP_LOGE(TAG, "Title is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(s_info_state.task_title, title, LCD_INFO_TITLE_MAX_LEN - 1);
    s_info_state.task_title[LCD_INFO_TITLE_MAX_LEN - 1] = '\0';

    ESP_LOGD(TAG, "Title set to: %s", s_info_state.task_title);
    return ESP_OK;
}

esp_err_t lcd_info_set_timer(uint32_t seconds)
{
    if (!s_info_state.initialized)
    {
        ESP_LOGW(TAG, "Info display not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    s_info_state.timer_seconds = seconds;

    ESP_LOGD(TAG, "Timer set to: %lu seconds", seconds);
    return ESP_OK;
}

void lcd_info_update(void)
{
    if (!s_info_state.initialized)
    {
        return;
    }

    lcd_fill_rect_buffer(0, 0, LCD_WIDTH, LCD_HEIGHT, LCD_COLOR_BLACK);

    lcd_draw_string_buffer(10, LCD_INFO_TITLE_Y, s_info_state.task_title, LCD_COLOR_WHITE,
                           LCD_COLOR_BLACK, LCD_INFO_TITLE_SIZE);

    char timer_str[LCD_INFO_TIMER_MAX_LEN];
    format_timer(s_info_state.timer_seconds, timer_str, sizeof(timer_str));
    lcd_draw_string_buffer(10, LCD_INFO_TIMER_Y, timer_str, LCD_COLOR_WHITE, LCD_COLOR_BLACK,
                           LCD_INFO_TIMER_SIZE);

    lcd_flush_buffer();
}

#else

esp_err_t lcd_info_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t lcd_info_deinit(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t lcd_info_set_title(const char* title)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t lcd_info_set_timer(uint32_t seconds)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void lcd_info_update(void)
{
}

#endif