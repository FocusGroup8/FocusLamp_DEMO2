/*
 * lcd_service.h - LCD display service for FocusLamp
 * High-level service with expression rendering, info display, and page management.
 */

#pragma once
#ifndef __LCD_SERVICE_H__
#define __LCD_SERVICE_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#include "lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== Mode System ===================== */

/** @brief Display mode enumeration */
typedef enum {
    LCD_MODE_FOCUS = 0,       /* 专注模式 - info page with task */
    LCD_MODE_COMPANION,       /* 陪伴模式 - happy expression */
    LCD_MODE_SILENT,          /* 静默模式 - sleepy expression */
    LCD_MODE_CUSTOM,          /* 自定义模式 - info page with custom name */
} lcd_mode_t;

/**
 * @brief Set display mode. Automatically switches page and expression.
 * @param mode Target mode
 */
void lcd_service_mode_set(lcd_mode_t mode);

/**
 * @brief Get current display mode.
 * @return lcd_mode_t
 */
lcd_mode_t lcd_service_mode_get(void);

/**
 * @brief Set custom mode name for LCD_MODE_CUSTOM.
 * @param name Mode name string
 */
void lcd_service_mode_set_name(const char *name);

/* ===================== Focus Task API ===================== */

/**
 * @brief Start a focus task with countdown timer.
 *        Switches to FOCUS mode on info page.
 * @param name    Task name string
 * @param seconds Countdown duration in seconds
 */
void lcd_service_task_start(const char *name, uint32_t seconds);

/**
 * @brief Stop current task and clear countdown.
 */
void lcd_service_task_stop(void);

/**
 * @brief Check if a task is currently running.
 * @return true if running
 */
bool lcd_service_task_is_running(void);

/* ===================== Public API ===================== */

/**
 * @brief Initialize LCD service.
 *        Subscribes to EV_LCD_* events and initializes lcd_driver.
 * @return esp_err_t
 */
esp_err_t lcd_service_init(void);

/**
 * @brief Unified periodic update.
 *        Dispatches to expression or info update based on current page.
 *        Call this from the LCD task loop instead of calling individual update functions.
 */
void lcd_service_update(void);

/**
 * @brief Set LCD backlight brightness.
 * @param brightness  Brightness level (0-255)
 */
void lcd_service_set_brightness(uint8_t brightness);

/**
 * @brief Put LCD to sleep (turn off display).
 */
void lcd_service_sleep(void);

/**
 * @brief Wake LCD from sleep (turn on display).
 */
void lcd_service_wakeup(void);

/* ===================== Native Expression API ===================== */

/**
 * @brief Initialize native expression renderer.
 * @return esp_err_t
 */
esp_err_t lcd_service_expression_init(void);

/**
 * @brief Deinitialize native expression renderer.
 * @return esp_err_t
 */
esp_err_t lcd_service_expression_deinit(void);

/**
 * @brief Update expression animation (call periodically).
 */
void lcd_service_expression_update(void);

/**
 * @brief Set expression by low-level type.
 * @param expr  Expression type
 * @return esp_err_t
 */
esp_err_t lcd_service_expression_set(lcd_expression_t expr);

/**
 * @brief Get current expression.
 * @return lcd_expression_t
 */
lcd_expression_t lcd_service_expression_get(void);

/**
 * @brief Enable or disable auto blink.
 * @param enable  true to enable, false to disable
 */
void lcd_service_expression_set_auto_blink(bool enable);

/**
 * @brief Check if auto blink is enabled.
 * @return bool
 */
bool lcd_service_expression_is_auto_blink(void);

/* ===================== Native Info Display API ===================== */

/**
 * @brief Initialize info display.
 * @return esp_err_t
 */
esp_err_t lcd_service_info_init(void);

/**
 * @brief Deinitialize info display.
 * @return esp_err_t
 */
esp_err_t lcd_service_info_deinit(void);

/**
 * @brief Set info display title text.
 * @param title  Title string
 * @return esp_err_t
 */
esp_err_t lcd_service_info_set_title(const char *title);

/**
 * @brief Set info display timer value.
 * @param seconds  Timer in seconds
 * @return esp_err_t
 */
esp_err_t lcd_service_info_set_timer(uint32_t seconds);

/**
 * @brief Update info display (redraw screen).
 */
void lcd_service_info_update(void);

/* ===================== Native Page Management API ===================== */

/**
 * @brief Initialize page manager.
 * @return esp_err_t
 */
esp_err_t lcd_service_page_init(void);

/**
 * @brief Deinitialize page manager.
 * @return esp_err_t
 */
esp_err_t lcd_service_page_deinit(void);

/**
 * @brief Switch to a specific native page.
 * @param page  Target native page
 * @return esp_err_t
 */
esp_err_t lcd_service_page_switch_to(lcd_page_t page);

/**
 * @brief Switch to next native page in sequence.
 * @return esp_err_t
 */
esp_err_t lcd_service_page_next(void);

/**
 * @brief Get current native page.
 * @return lcd_page_t
 */
lcd_page_t lcd_service_page_get_current(void);

/**
 * @brief Register page switch callback.
 * @param callback  Callback function for page switch events
 */
void lcd_service_page_register_callback(void (*callback)(lcd_page_t page));

#ifdef __cplusplus
}
#endif

#endif /* __LCD_SERVICE_H__ */
