#include "lcd_module.h"

#if (LCD_MODULE_ENABLE == 1)

#include "esp_log.h"

#include "lcd_config.h"
#include "lcd_driver.h"
#include "lcd_expression.h"
#include "lcd_info_display.h"
#include "lcd_page_manager.h"

static const char* TAG = "LCD_MODULE";

#if (LCD_MODULE_LOG_ENABLE == 1)
#define LCD_LOG_ENABLED 1
#else
#define LCD_LOG_ENABLED 0
#endif

#define LCD_LOGI(fmt, ...)                        \
    do                                            \
    {                                             \
        if (LCD_LOG_ENABLED && s_lcd_log_enabled) \
            ESP_LOGI(TAG, fmt, ##__VA_ARGS__);    \
    } while (0)
#define LCD_LOGD(fmt, ...)                        \
    do                                            \
    {                                             \
        if (LCD_LOG_ENABLED && s_lcd_log_enabled) \
            ESP_LOGD(TAG, fmt, ##__VA_ARGS__);    \
    } while (0)

static bool s_lcd_log_enabled = false;

typedef struct
{
    bool                 initialized;
    lcd_event_callback_t event_callback;
} lcd_module_t;

static lcd_module_t s_lcd_module = {
    .initialized    = false,
    .event_callback = NULL,
};

static void on_page_switch(lcd_page_t page)
{
    LCD_LOGI("Page switched to %d", page);

    if (page == LCD_PAGE_EXPRESSION)
    {
        lcd_expression_init();
    }
    else if (page == LCD_PAGE_INFO)
    {
        lcd_info_init();
    }
}

esp_err_t lcd_module_init(void)
{
    if (s_lcd_module.initialized)
    {
        ESP_LOGW(TAG, "LCD module already initialized");
        return ESP_OK;
    }

    esp_err_t ret = lcd_driver_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize LCD driver: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = lcd_page_manager_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize page manager: %s", esp_err_to_name(ret));
        return ret;
    }

    lcd_page_manager_register_callback(on_page_switch);

    ret = lcd_expression_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize expression renderer: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = lcd_info_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize info display: %s", esp_err_to_name(ret));
        return ret;
    }

    s_lcd_module.initialized    = true;
    s_lcd_module.event_callback = NULL;

    s_lcd_log_enabled = false;

    ESP_LOGI(TAG, "LCD module initialized");
    return ESP_OK;
}

esp_err_t lcd_module_deinit(void)
{
    if (!s_lcd_module.initialized)
    {
        ESP_LOGW(TAG, "LCD module not initialized");
        return ESP_OK;
    }

    lcd_info_deinit();
    lcd_expression_deinit();
    lcd_page_manager_deinit();
    lcd_driver_deinit();

    s_lcd_module.initialized    = false;
    s_lcd_module.event_callback = NULL;

    ESP_LOGI(TAG, "LCD module deinitialized");
    return ESP_OK;
}

void lcd_module_update(void)
{
    if (!s_lcd_module.initialized)
    {
        return;
    }

    lcd_page_t current_page = lcd_page_manager_get_current();

    if (current_page == LCD_PAGE_EXPRESSION)
    {
        lcd_expression_update();
    }
    else if (current_page == LCD_PAGE_INFO)
    {
        lcd_info_update();
    }
}

esp_err_t lcd_module_switch_page(lcd_page_t page)
{
    if (!s_lcd_module.initialized)
    {
        ESP_LOGW(TAG, "LCD module not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    return lcd_page_manager_switch_to(page);
}

esp_err_t lcd_module_next_page(void)
{
    if (!s_lcd_module.initialized)
    {
        ESP_LOGW(TAG, "LCD module not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    return lcd_page_manager_next();
}

lcd_page_t lcd_module_get_current_page(void)
{
    return lcd_page_manager_get_current();
}

esp_err_t lcd_module_set_expression(lcd_expression_t expr)
{
    if (!s_lcd_module.initialized)
    {
        ESP_LOGW(TAG, "LCD module not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    return lcd_expression_set(expr);
}

lcd_expression_t lcd_module_get_expression(void)
{
    return lcd_expression_get();
}

void lcd_module_set_auto_blink(bool enable)
{
    lcd_expression_set_auto_blink(enable);
}

bool lcd_module_is_auto_blink(void)
{
    return lcd_expression_is_auto_blink();
}

esp_err_t lcd_module_set_info_title(const char* title)
{
    if (!s_lcd_module.initialized)
    {
        ESP_LOGW(TAG, "LCD module not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    return lcd_info_set_title(title);
}

esp_err_t lcd_module_set_info_timer(uint32_t seconds)
{
    if (!s_lcd_module.initialized)
    {
        ESP_LOGW(TAG, "LCD module not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    return lcd_info_set_timer(seconds);
}

esp_err_t lcd_module_register_callback(lcd_event_callback_t callback)
{
    if (!s_lcd_module.initialized)
    {
        ESP_LOGW(TAG, "LCD module not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    s_lcd_module.event_callback = callback;
    return ESP_OK;
}

void lcd_module_send_event(const lcd_event_t* event)
{
    if (!s_lcd_module.initialized || event == NULL)
    {
        return;
    }

    lcd_page_t current_page = lcd_page_manager_get_current();

    if (current_page == LCD_PAGE_EXPRESSION)
    {
        switch (event->type)
        {
        case LCD_EVENT_HUMAN_PRESENCE:
            if (event->data.is_present)
            {
                lcd_expression_set(LCD_EXPRESSION_NORMAL);
            }
            else
            {
                lcd_expression_set(LCD_EXPRESSION_SLEEPY);
            }
            break;

        case LCD_EVENT_MOTION:
            lcd_expression_set(LCD_EXPRESSION_SURPRISED);
            break;

        default:
            break;
        }
    }
    else if (current_page == LCD_PAGE_INFO)
    {
        switch (event->type)
        {
        case LCD_EVENT_HUMAN_PRESENCE:
            if (event->data.is_present)
            {
                lcd_info_set_title("Detecting");
            }
            else
            {
                lcd_info_set_title("Idle");
            }
            break;

        default:
            break;
        }
    }

    if (s_lcd_module.event_callback != NULL)
    {
        s_lcd_module.event_callback(event);
    }
}

void lcd_module_enable_log(void)
{
#if (LCD_LOG_ENABLED == 1)
    s_lcd_log_enabled = true;
    ESP_LOGI(TAG, "LCD module log enabled");
#else
    ESP_LOGW(TAG, "LCD module log is disabled in Kconfig");
#endif
}

void lcd_module_disable_log(void)
{
#if (LCD_LOG_ENABLED == 1)
    s_lcd_log_enabled = false;
    ESP_LOGI(TAG, "LCD module log disabled");
#else
    ESP_LOGW(TAG, "LCD module log is disabled in Kconfig");
#endif
}

bool lcd_module_is_log_enabled(void)
{
    return s_lcd_log_enabled;
}

#endif