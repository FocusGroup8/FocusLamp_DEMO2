#ifndef LCD_EXPRESSION_H
#define LCD_EXPRESSION_H

#include "esp_err.h"

#include "lcd_eye_config.h"
#include "lcd_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

    esp_err_t lcd_expression_init(void);

    esp_err_t lcd_expression_deinit(void);

    void lcd_expression_update(void);

    esp_err_t lcd_expression_set(lcd_expression_t expr);

    lcd_expression_t lcd_expression_get(void);

    void lcd_expression_set_auto_blink(bool enable);

    bool lcd_expression_is_auto_blink(void);

#ifdef __cplusplus
}
#endif

#endif