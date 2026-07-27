#ifndef LVGL_DISP_H
#define LVGL_DISP_H

#include "esp_err.h"

#include "lvgl_ui_config.h"

#if (LVGL_UI_ENABLE == 1)

#include "lvgl.h"

#ifdef __cplusplus
extern "C"
{
#endif

    esp_err_t lvgl_disp_init(void);

    esp_err_t lvgl_disp_deinit(void);

    lv_display_t* lvgl_disp_get(void);

#ifdef __cplusplus
}
#endif

#endif

#endif
