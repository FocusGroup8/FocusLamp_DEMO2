#ifndef LCD_PAGE_MANAGER_H
#define LCD_PAGE_MANAGER_H

#include "esp_err.h"

#include "lcd_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef void (*lcd_page_switch_cb_t)(lcd_page_t page);

    esp_err_t lcd_page_manager_init(void);

    esp_err_t lcd_page_manager_deinit(void);

    esp_err_t lcd_page_manager_switch_to(lcd_page_t page);

    esp_err_t lcd_page_manager_next(void);

    lcd_page_t lcd_page_manager_get_current(void);

    void lcd_page_manager_register_callback(lcd_page_switch_cb_t callback);

#ifdef __cplusplus
}
#endif

#endif