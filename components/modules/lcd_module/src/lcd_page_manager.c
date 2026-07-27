#include "lcd_page_manager.h"

#include "esp_log.h"

#include "lcd_config.h"
#include "lcd_driver.h"

#if (RADAR_ENABLE_LCD_DISPLAY == 1)

static const char* TAG = "LCD_PAGE_MANAGER";

typedef struct
{
    lcd_page_t           current_page;
    lcd_page_switch_cb_t on_page_switch;
    bool                 initialized;
} lcd_page_manager_t;

static lcd_page_manager_t s_page_manager = {
    .current_page   = LCD_PAGE_EXPRESSION,
    .on_page_switch = NULL,
    .initialized    = false,
};

esp_err_t lcd_page_manager_init(void)
{
    if (s_page_manager.initialized)
    {
        ESP_LOGW(TAG, "Page manager already initialized");
        return ESP_OK;
    }

    s_page_manager.current_page   = LCD_PAGE_EXPRESSION;
    s_page_manager.on_page_switch = NULL;
    s_page_manager.initialized    = true;

    ESP_LOGI(TAG, "Page manager initialized");
    return ESP_OK;
}

esp_err_t lcd_page_manager_deinit(void)
{
    if (!s_page_manager.initialized)
    {
        ESP_LOGW(TAG, "Page manager not initialized");
        return ESP_OK;
    }

    s_page_manager.current_page   = LCD_PAGE_EXPRESSION;
    s_page_manager.on_page_switch = NULL;
    s_page_manager.initialized    = false;

    ESP_LOGI(TAG, "Page manager deinitialized");
    return ESP_OK;
}

esp_err_t lcd_page_manager_switch_to(lcd_page_t page)
{
    if (!s_page_manager.initialized)
    {
        ESP_LOGW(TAG, "Page manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (page >= LCD_PAGE_COUNT)
    {
        ESP_LOGE(TAG, "Invalid page: %d", page);
        return ESP_ERR_INVALID_ARG;
    }

    if (s_page_manager.current_page == page)
    {
        ESP_LOGD(TAG, "Already on page %d", page);
        return ESP_OK;
    }

    s_page_manager.current_page = page;

    ESP_LOGI(TAG, "Switched to page %d", page);

    if (s_page_manager.on_page_switch != NULL)
    {
        s_page_manager.on_page_switch(page);
    }

    return ESP_OK;
}

esp_err_t lcd_page_manager_next(void)
{
    if (!s_page_manager.initialized)
    {
        ESP_LOGW(TAG, "Page manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    lcd_page_t next_page = (lcd_page_t)((s_page_manager.current_page + 1) % LCD_PAGE_COUNT);
    return lcd_page_manager_switch_to(next_page);
}

lcd_page_t lcd_page_manager_get_current(void)
{
    return s_page_manager.current_page;
}

void lcd_page_manager_register_callback(lcd_page_switch_cb_t callback)
{
    s_page_manager.on_page_switch = callback;
}

#else

esp_err_t lcd_page_manager_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t lcd_page_manager_deinit(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t lcd_page_manager_switch_to(lcd_page_t page)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t lcd_page_manager_next(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

lcd_page_t lcd_page_manager_get_current(void)
{
    return LCD_PAGE_EXPRESSION;
}

void lcd_page_manager_register_callback(lcd_page_switch_cb_t callback)
{
}

#endif