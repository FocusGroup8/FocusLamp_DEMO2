#ifndef LVGL_INFO_H
#define LVGL_INFO_H

#include "esp_err.h"

#include "lvgl_ui_config.h"

#if (LVGL_UI_ENABLE == 1) && (LVGL_UI_ENABLE_INFO == 1)

#include "lvgl.h"

#ifdef __cplusplus
extern "C"
{
#endif

    esp_err_t lvgl_info_init(lv_obj_t* parent);

    void lvgl_info_deinit(void);

    void lvgl_info_update(void);

    esp_err_t lvgl_info_set_title(const char* title);

    esp_err_t lvgl_info_set_value(int value);

    esp_err_t lvgl_info_set_progress(int progress);

    esp_err_t lvgl_info_set_status(bool status);

    esp_err_t lvgl_info_set_extra_value(int value);

    esp_err_t lvgl_info_set_time(int seconds);

#ifdef __cplusplus
}
#endif

#endif

#endif
