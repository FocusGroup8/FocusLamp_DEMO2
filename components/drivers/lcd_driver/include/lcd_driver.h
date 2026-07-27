/*
 * lcd_driver.h - LCD display driver for FocusLamp
 * SPI-based LCD with DC/CS/BL control pins.
 */

#pragma once
#ifndef __LCD_DRIVER_H__
#define __LCD_DRIVER_H__

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#include "lcd_types.h"
#include "lcd_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* LCD_WIDTH and LCD_HEIGHT are defined in lcd_config.h */

/**
 * @brief Initialize LCD driver.
 *        Calls bsp_spi_init() for SPI bus, configures DC/CS/BL GPIOs.
 * @return esp_err_t
 */
esp_err_t lcd_driver_init(void);

/**
 * @brief Deinitialize LCD driver.
 * @return esp_err_t
 */
esp_err_t lcd_driver_deinit(void);

/**
 * @brief Write a command byte to LCD.
 * @param cmd  Command byte
 */
void lcd_driver_write_command(uint8_t cmd);

/**
 * @brief Write data bytes to LCD.
 * @param data  Pointer to data buffer
 * @param len   Number of bytes to write
 */
void lcd_driver_write_data(uint8_t *data, uint16_t len);

/**
 * @brief Set the display area (window) for subsequent data writes.
 * @param x1  Start X coordinate
 * @param y1  Start Y coordinate
 * @param x2  End X coordinate
 * @param y2  End Y coordinate
 */
void lcd_driver_set_area(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);

/**
 * @brief Turn display on.
 * @return esp_err_t
 */
esp_err_t lcd_driver_display_on(void);

/**
 * @brief Turn display off.
 * @return esp_err_t
 */
esp_err_t lcd_driver_display_off(void);

/**
 * @brief Set backlight brightness via PWM.
 * @param brightness  Brightness level (0-255)
 */
void lcd_driver_set_backlight(uint8_t brightness);

/* ===================== Frame Buffer Drawing Functions ===================== */

/**
 * @brief Fill the entire screen with a color (direct SPI write).
 * @param color  RGB565 color value
 */
void lcd_fill_screen(lcd_color_t color);

/**
 * @brief Fill a rectangle directly to LCD (direct SPI write).
 * @param x      X start coordinate
 * @param y      Y start coordinate
 * @param w      Width in pixels
 * @param h      Height in pixels
 * @param color  RGB565 color value
 */
void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, lcd_color_t color);

/**
 * @brief Draw a horizontal line to the frame buffer.
 * @param x      X start coordinate
 * @param y      Y coordinate
 * @param width  Width in pixels
 * @param color  RGB565 color value
 */
void lcd_draw_hline_buffer(uint16_t x, uint16_t y, uint16_t width, lcd_color_t color);

/**
 * @brief Fill a rectangle in the frame buffer.
 * @param x      X start coordinate
 * @param y      Y start coordinate
 * @param w      Width in pixels
 * @param h      Height in pixels
 * @param color  RGB565 color value
 */
void lcd_fill_rect_buffer(uint16_t x, uint16_t y, uint16_t w, uint16_t h, lcd_color_t color);

/**
 * @brief Flush the entire frame buffer to the LCD.
 */
void lcd_flush_buffer(void);

/**
 * @brief Draw a character at specified position (direct SPI write).
 * @param x      X coordinate
 * @param y      Y coordinate
 * @param c      ASCII character
 * @param color  Foreground color
 * @param bg     Background color
 * @param size   Font size multiplier
 */
void lcd_draw_char(uint16_t x, uint16_t y, char c, lcd_color_t color, lcd_color_t bg, uint8_t size);

/**
 * @brief Draw a character into the frame buffer.
 * @param x      X coordinate
 * @param y      Y coordinate
 * @param c      ASCII character
 * @param color  Foreground color
 * @param bg     Background color
 * @param size   Font size multiplier
 */
void lcd_draw_char_buffer(uint16_t x, uint16_t y, char c, lcd_color_t color, lcd_color_t bg, uint8_t size);

/**
 * @brief Draw a string into the frame buffer.
 * @param x      X start coordinate
 * @param y      Y start coordinate
 * @param str    Null-terminated string
 * @param color  Foreground color
 * @param bg     Background color
 * @param size   Font size multiplier
 */
void lcd_draw_string_buffer(uint16_t x, uint16_t y, const char *str, lcd_color_t color, lcd_color_t bg, uint8_t size);

/**
 * @brief Draw a 16x16 Chinese character into the frame buffer.
 * @param x       X start coordinate
 * @param y       Y start coordinate
 * @param bitmap  16x16 bitmap data (16 rows of uint16_t)
 * @param color   Foreground color
 * @param bg      Background color
 */
void lcd_draw_cn_char_buffer(uint16_t x, uint16_t y, const uint16_t *bitmap,
                             lcd_color_t color, lcd_color_t bg);

/**
 * @brief Draw a UTF-8 mixed Chinese/ASCII string into the frame buffer.
 *        Chinese characters use 16x16 font, ASCII uses 8xN bitmap.
 * @param x           X start coordinate
 * @param y           Y start coordinate
 * @param str         UTF-8 encoded string
 * @param color       Foreground color
 * @param bg          Background color
 * @param ascii_size  ASCII font size multiplier
 */
void lcd_draw_utf8_string_buffer(uint16_t x, uint16_t y, const char *str,
                                 lcd_color_t color, lcd_color_t bg, uint8_t ascii_size);

/**
 * @brief Draw a string directly to LCD (direct SPI write).
 * @param x      X start coordinate
 * @param y      Y start coordinate
 * @param str    Null-terminated string
 * @param color  Foreground color
 * @param bg     Background color
 * @param size   Font size multiplier
 */
void lcd_draw_string(uint16_t x, uint16_t y, const char *str, lcd_color_t color, lcd_color_t bg, uint8_t size);

/**
 * @brief Set backlight brightness in percent (0-100).
 * @param brightness  Brightness percentage
 * @return esp_err_t
 */
esp_err_t lcd_set_brightness(uint8_t brightness);

/**
 * @brief Flush a specific area from a data buffer to the LCD.
 * @param x1          Start X
 * @param y1          Start Y
 * @param x2          End X
 * @param y2          End Y
 * @param color_data  Pixel color data (RGB565 packed)
 * @param len         Length of data in bytes
 */
void lcd_driver_flush_area(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2,
                           const uint8_t *color_data, size_t len);

/**
 * @brief Diagnostic: Print backlight status.
 * @param from  Caller identifier string
 */
void lcd_driver_diag_backlight(const char *from);

/**
 * @brief Diagnostic: Print SPI status.
 * @param from  Caller identifier string
 */
void lcd_driver_diag_spi(const char *from);

#ifdef __cplusplus
}
#endif

#endif /* __LCD_DRIVER_H__ */