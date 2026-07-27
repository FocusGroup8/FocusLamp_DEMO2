#ifndef LVGL_UI_TYPES_H
#define LVGL_UI_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        LVGL_UI_PAGE_EXPRESSION = 0,
        LVGL_UI_PAGE_INFO       = 1,
        LVGL_UI_PAGE_RADAR_VIEW = 2,
        LVGL_UI_PAGE_COUNT,
    } lvgl_ui_page_t;

    typedef enum
    {
        LVGL_UI_EXPR_NORMAL = 0,
        LVGL_UI_EXPR_HAPPY,
        LVGL_UI_EXPR_SAD,
        LVGL_UI_EXPR_ANGRY,
        LVGL_UI_EXPR_SURPRISED,
        LVGL_UI_EXPR_SLEEPY,
        LVGL_UI_EXPR_COUNT,
    } lvgl_ui_expression_t;

    typedef enum
    {
        LVGL_UI_DISPLAY_MODE_IDLE = 0,
        LVGL_UI_DISPLAY_MODE_BREATH,
        LVGL_UI_DISPLAY_MODE_HEART_RATE,
        LVGL_UI_DISPLAY_MODE_GESTURE,
    } lvgl_ui_display_mode_t;

    typedef enum
    {
        LVGL_UI_EVENT_HUMAN_PRESENCE = 0,
        LVGL_UI_EVENT_BREATH_RATE,
        LVGL_UI_EVENT_HEART_RATE,
        LVGL_UI_EVENT_GESTURE,
        LVGL_UI_EVENT_MOTION,
        LVGL_UI_EVENT_COUNT,
    } lvgl_ui_event_type_t;

    typedef enum
    {
        LVGL_UI_CHART_BREATH = 0,
        LVGL_UI_CHART_HEART,
        LVGL_UI_CHART_BOTH,
    } lvgl_ui_chart_type_t;

    typedef struct
    {
        lvgl_ui_event_type_t type;
        union
        {
            bool    is_present;
            float   breath_rate;
            float   heart_rate;
            uint8_t gesture;
            int16_t motion_data[3];
        } data;
    } lvgl_ui_event_t;

    typedef void (*lvgl_ui_event_callback_t)(const lvgl_ui_event_t* event);

#ifdef __cplusplus
}
#endif

#endif
