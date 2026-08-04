#ifndef LVGL_UI_H
#define LVGL_UI_H

#include "esp_err.h"

#include "lvgl_ui_config.h"
#include "lvgl_ui_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

#if (LVGL_UI_ENABLE == 1)

    esp_err_t lvgl_ui_init(void);

    esp_err_t lvgl_ui_deinit(void);

    void lvgl_ui_update(void);

    esp_err_t lvgl_ui_switch_page(lvgl_ui_page_t page);

    esp_err_t lvgl_ui_next_page(void);

    lvgl_ui_page_t lvgl_ui_get_current_page(void);

    esp_err_t lvgl_ui_set_expression(lvgl_ui_expression_t expr);

    lvgl_ui_expression_t lvgl_ui_get_expression(void);

    void lvgl_ui_set_auto_blink(bool enable);

    bool lvgl_ui_is_auto_blink(void);

    esp_err_t lvgl_ui_set_info_title(const char* title);

    esp_err_t lvgl_ui_set_info_value(int value);

    esp_err_t lvgl_ui_set_info_progress(int progress);

    esp_err_t lvgl_ui_set_info_status(bool status);

    esp_err_t lvgl_ui_set_info_timer(uint32_t seconds);

    esp_err_t lvgl_ui_register_callback(lvgl_ui_event_callback_t callback);

    void lvgl_ui_send_event(const lvgl_ui_event_t* event);

    void lvgl_ui_set_radar_data(float breath_rate, float heart_rate, bool is_present);

#else

static inline esp_err_t lvgl_ui_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t lvgl_ui_deinit(void)
{
    return ESP_OK;
}
static inline void lvgl_ui_update(void)
{
}
static inline esp_err_t lvgl_ui_switch_page(lvgl_ui_page_t page)
{
    (void)page;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t lvgl_ui_next_page(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline lvgl_ui_page_t lvgl_ui_get_current_page(void)
{
    return LVGL_UI_PAGE_EXPRESSION;
}
static inline esp_err_t lvgl_ui_set_expression(lvgl_ui_expression_t expr)
{
    (void)expr;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline lvgl_ui_expression_t lvgl_ui_get_expression(void)
{
    return LVGL_UI_EXPR_NORMAL;
}
static inline void lvgl_ui_set_auto_blink(bool enable)
{
    (void)enable;
}
static inline bool lvgl_ui_is_auto_blink(void)
{
    return false;
}
static inline esp_err_t lvgl_ui_set_info_title(const char* title)
{
    (void)title;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t lvgl_ui_set_info_value(int value)
{
    (void)value;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t lvgl_ui_set_info_progress(int progress)
{
    (void)progress;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t lvgl_ui_set_info_status(bool status)
{
    (void)status;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t lvgl_ui_set_info_timer(uint32_t seconds)
{
    (void)seconds;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t lvgl_ui_register_callback(lvgl_ui_event_callback_t callback)
{
    (void)callback;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline void lvgl_ui_send_event(const lvgl_ui_event_t* event)
{
    (void)event;
}
static inline void lvgl_ui_set_radar_data(float breath_rate, float heart_rate, bool is_present)
{
    (void)breath_rate;
    (void)heart_rate;
    (void)is_present;
}

#endif

#ifdef __cplusplus
}
#endif

#endif
