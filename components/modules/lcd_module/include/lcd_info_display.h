#ifndef LCD_INFO_DISPLAY_H
#define LCD_INFO_DISPLAY_H

#include "esp_err.h"

#include "lcd_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define LCD_INFO_TITLE_MAX_LEN 16
#define LCD_INFO_TIMER_MAX_LEN 8

    esp_err_t lcd_info_init(void);

    esp_err_t lcd_info_deinit(void);

    esp_err_t lcd_info_set_title(const char* title);

    esp_err_t lcd_info_set_timer(uint32_t seconds);

    void lcd_info_update(void);

#ifdef __cplusplus
}
#endif

#endif