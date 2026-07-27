#ifndef LVGL_EXPRESSION_H
#define LVGL_EXPRESSION_H

#include "esp_err.h"

#include "lvgl_ui_config.h"
#include "lvgl_ui_types.h"

#if (LVGL_UI_ENABLE == 1) && (LVGL_UI_ENABLE_EXPRESSION == 1)

#include "lvgl.h"

#ifdef __cplusplus
extern "C"
{
#endif

    esp_err_t lvgl_expression_init(lv_obj_t* parent);

    void lvgl_expression_deinit(void);

    void lvgl_expression_update(void);

    esp_err_t lvgl_expression_set(lvgl_ui_expression_t expr);

    lvgl_ui_expression_t lvgl_expression_get(void);

    void lvgl_expression_set_auto_blink(bool enable);

    bool lvgl_expression_is_auto_blink(void);

#ifdef __cplusplus
}
#endif

#endif

#endif
