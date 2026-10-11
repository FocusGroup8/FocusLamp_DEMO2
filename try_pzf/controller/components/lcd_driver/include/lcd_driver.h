#pragma once

#include "esp_err.h"
#include "lcd_types.h"
#include "lcd_config.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t lcd_driver_init(void);
/* Stop the UI caller before deinit. A DMA timeout retains resources for retry. */
esp_err_t lcd_driver_deinit(void);
/* Serialized with refresh; on restores backlight, off disables it.
 * GPIO51 backlight is DIGITAL: 0 off, any nonzero value on (not PWM). */
esp_err_t lcd_driver_display_on(void);
esp_err_t lcd_driver_display_off(void);
esp_err_t lcd_driver_set_backlight(uint8_t brightness);

/* The status UI is the sole logical-frame writer. These functions do no I/O. */
void lcd_fill_rect_buffer(uint16_t x, uint16_t y, uint16_t w, uint16_t h, lcd_color_t color);
void lcd_draw_char_buffer(uint16_t x, uint16_t y, char c, lcd_color_t color, lcd_color_t bg, uint8_t size);
void lcd_draw_string_buffer(uint16_t x, uint16_t y, const char *text, lcd_color_t color, lcd_color_t bg, uint8_t size);
void lcd_draw_utf8_string_buffer(uint16_t x, uint16_t y, const char *text, lcd_color_t color, lcd_color_t bg, uint8_t size);
/* Waits for completion; only ESP_OK permits another frame. */
esp_err_t lcd_flush_buffer(void);

#ifdef __cplusplus
}
#endif
