#ifndef LCD_MODULE_H
#define LCD_MODULE_H

#include "esp_err.h"

#include "lcd_module_config.h"
#include "lcd_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        LCD_EVENT_HUMAN_PRESENCE = 0,
        LCD_EVENT_MOTION,
        LCD_EVENT_COUNT,
    } lcd_event_type_t;

    typedef struct
    {
        lcd_event_type_t type;
        union
        {
            bool    is_present;
            int16_t motion_data[3];
        } data;
    } lcd_event_t;

    typedef void (*lcd_event_callback_t)(const lcd_event_t* event);

#if (LCD_MODULE_ENABLE == 1)

    esp_err_t lcd_module_init(void);

    esp_err_t lcd_module_deinit(void);

    void lcd_module_update(void);

    esp_err_t lcd_module_switch_page(lcd_page_t page);

    esp_err_t lcd_module_next_page(void);

    lcd_page_t lcd_module_get_current_page(void);

    esp_err_t lcd_module_set_expression(lcd_expression_t expr);

    lcd_expression_t lcd_module_get_expression(void);

    void lcd_module_set_auto_blink(bool enable);

    bool lcd_module_is_auto_blink(void);

    esp_err_t lcd_module_set_info_title(const char* title);

    esp_err_t lcd_module_set_info_timer(uint32_t seconds);

    esp_err_t lcd_module_register_callback(lcd_event_callback_t callback);

    void lcd_module_send_event(const lcd_event_t* event);

    void lcd_module_enable_log(void);
    void lcd_module_disable_log(void);
    bool lcd_module_is_log_enabled(void);

#else

static inline esp_err_t lcd_module_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t lcd_module_deinit(void)
{
    return ESP_OK;
}
static inline void lcd_module_update(void)
{
}
static inline esp_err_t lcd_module_switch_page(lcd_page_t page)
{
    (void)page;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t lcd_module_next_page(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline lcd_page_t lcd_module_get_current_page(void)
{
    return LCD_PAGE_EXPRESSION;
}
static inline esp_err_t lcd_module_set_expression(lcd_expression_t expr)
{
    (void)expr;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline lcd_expression_t lcd_module_get_expression(void)
{
    return LCD_EXPRESSION_NORMAL;
}
static inline void lcd_module_set_auto_blink(bool enable)
{
    (void)enable;
}
static inline bool lcd_module_is_auto_blink(void)
{
    return false;
}
static inline esp_err_t lcd_module_set_info_title(const char* title)
{
    (void)title;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t lcd_module_set_info_timer(uint32_t seconds)
{
    (void)seconds;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t lcd_module_register_callback(lcd_event_callback_t callback)
{
    (void)callback;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline void lcd_module_send_event(const lcd_event_t* event)
{
    (void)event;
}

static inline void lcd_module_enable_log(void)
{
}
static inline void lcd_module_disable_log(void)
{
}
static inline bool lcd_module_is_log_enabled(void)
{
    return false;
}

#endif

#ifdef __cplusplus
}
#endif

#endif