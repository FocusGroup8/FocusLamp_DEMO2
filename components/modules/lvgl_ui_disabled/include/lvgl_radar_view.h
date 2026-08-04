#ifndef LVGL_RADAR_VIEW_H
#define LVGL_RADAR_VIEW_H

#include "esp_err.h"

#include "lvgl_ui_config.h"
#include "lvgl_ui_types.h"

#if (LVGL_UI_ENABLE == 1) && (LVGL_UI_ENABLE_RADAR_VIEW == 1)

#include "lvgl.h"

#ifdef __cplusplus
extern "C"
{
#endif

    esp_err_t lvgl_radar_view_init(lv_obj_t* parent);

    void lvgl_radar_view_deinit(void);

    void lvgl_radar_view_update(void);

    void lvgl_radar_view_set_data(float breath_rate, float heart_rate, bool is_present);

    void lvgl_radar_view_add_breath_point(float value);

    void lvgl_radar_view_add_heart_point(float value);

    void lvgl_radar_view_set_chart_type(lvgl_ui_chart_type_t type);

    lvgl_ui_chart_type_t lvgl_radar_view_get_chart_type(void);

#ifdef __cplusplus
}
#endif

#endif

#endif
