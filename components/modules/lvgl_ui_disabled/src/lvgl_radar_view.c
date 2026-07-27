#include "lvgl_radar_view.h"

#include <stdio.h>

#include "esp_log.h"

#include "lvgl_ui_config.h"

#if (LVGL_UI_ENABLE == 1) && (LVGL_UI_ENABLE_RADAR_VIEW == 1)

static const char* TAG = "LVGL_RADAR";

static lv_obj_t*          s_screen         = NULL;
static lv_obj_t*          s_presence_label = NULL;
static lv_obj_t*          s_breath_label   = NULL;
static lv_obj_t*          s_heart_label    = NULL;
static lv_obj_t*          s_chart          = NULL;
static lv_chart_series_t* s_breath_series  = NULL;
static lv_chart_series_t* s_heart_series   = NULL;

static float                s_breath_rate = 0.0f;
static float                s_heart_rate  = 0.0f;
static bool                 s_is_present  = false;
static lvgl_ui_chart_type_t s_chart_type  = LVGL_UI_CHART_BREATH;

#define CHART_POINT_COUNT 20

esp_err_t lvgl_radar_view_init(lv_obj_t* parent)
{
    if (s_screen != NULL)
    {
        ESP_LOGW(TAG, "Radar view already initialized");
        return ESP_OK;
    }

    if (parent == NULL)
    {
        ESP_LOGE(TAG, "Parent is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    s_screen = parent;

    lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    ESP_LOGI(TAG, "Creating presence label...");
    s_presence_label = lv_label_create(s_screen);
    if (s_presence_label == NULL)
    {
        ESP_LOGE(TAG, "Failed to create presence label");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Presence label created: %p", s_presence_label);
    lv_label_set_text(s_presence_label, "-");
    lv_obj_set_pos(s_presence_label, 2, 2);
    lv_obj_set_style_bg_opa(s_presence_label, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(s_presence_label, lv_color_make(0x00, 0xFF, 0xFF), 0);

    ESP_LOGI(TAG, "Creating breath label...");
    s_breath_label = lv_label_create(s_screen);
    if (s_breath_label == NULL)
    {
        ESP_LOGE(TAG, "Failed to create breath label");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Breath label created: %p", s_breath_label);
    lv_label_set_text(s_breath_label, "BR:0");
    lv_obj_set_pos(s_breath_label, 30, 2);
    lv_obj_set_style_bg_opa(s_breath_label, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(s_breath_label, lv_color_make(0x00, 0xFF, 0xFF), 0);

    ESP_LOGI(TAG, "Creating heart label...");
    s_heart_label = lv_label_create(s_screen);
    if (s_heart_label == NULL)
    {
        ESP_LOGE(TAG, "Failed to create heart label");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Heart label created: %p", s_heart_label);
    lv_label_set_text(s_heart_label, "HR:0");
    lv_obj_set_pos(s_heart_label, 100, 2);
    lv_obj_set_style_bg_opa(s_heart_label, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(s_heart_label, lv_color_make(0x00, 0xFF, 0xFF), 0);

    ESP_LOGI(TAG, "Creating chart...");
    s_chart = lv_chart_create(s_screen);
    if (s_chart == NULL)
    {
        ESP_LOGE(TAG, "Failed to create chart");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Chart created: %p", s_chart);
    lv_obj_set_pos(s_chart, 2, 18);
    lv_obj_set_size(s_chart, 156, 40);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, CHART_POINT_COUNT);
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 40);
    lv_obj_set_style_bg_color(s_chart, lv_color_make(0x10, 0x10, 0x10), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_chart, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_chart, lv_color_make(0x40, 0x40, 0x40), LV_PART_MAIN);

    s_breath_series =
        lv_chart_add_series(s_chart, lv_color_make(0x00, 0xFF, 0x00), LV_CHART_AXIS_PRIMARY_Y);
    s_heart_series =
        lv_chart_add_series(s_chart, lv_color_make(0xFF, 0x00, 0x00), LV_CHART_AXIS_PRIMARY_Y);

    if (s_breath_series == NULL || s_heart_series == NULL)
    {
        ESP_LOGE(TAG, "Failed to add chart series");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Radar view initialized");
    return ESP_OK;
}

void lvgl_radar_view_deinit(void)
{
    if (s_chart != NULL)
    {
        lv_obj_delete_async(s_chart);
        s_chart         = NULL;
        s_breath_series = NULL;
        s_heart_series  = NULL;
    }

    if (s_heart_label != NULL)
    {
        lv_obj_delete_async(s_heart_label);
        s_heart_label = NULL;
    }

    if (s_breath_label != NULL)
    {
        lv_obj_delete_async(s_breath_label);
        s_breath_label = NULL;
    }

    if (s_presence_label != NULL)
    {
        lv_obj_delete_async(s_presence_label);
        s_presence_label = NULL;
    }

    s_screen = NULL;

    ESP_LOGI(TAG, "Radar view deinitialized");
}

void lvgl_radar_view_update(void)
{
    if (s_presence_label != NULL)
    {
        lv_label_set_text(s_presence_label, s_is_present ? "*" : "-");
    }

    if (s_breath_label != NULL)
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "BR:%.0f", s_breath_rate);
        lv_label_set_text(s_breath_label, buf);
    }

    if (s_heart_label != NULL)
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "HR:%.0f", s_heart_rate);
        lv_label_set_text(s_heart_label, buf);
    }
}

void lvgl_radar_view_set_data(float breath_rate, float heart_rate, bool is_present)
{
    s_breath_rate = breath_rate;
    s_heart_rate  = heart_rate;
    s_is_present  = is_present;
}

void lvgl_radar_view_add_breath_point(float value)
{
    s_breath_rate = value;
    if (s_chart != NULL && s_breath_series != NULL)
    {
        if (s_chart_type == LVGL_UI_CHART_BREATH || s_chart_type == LVGL_UI_CHART_BOTH)
        {
            lv_chart_set_next_value(s_chart, s_breath_series, (int32_t)value);
        }
    }
}

void lvgl_radar_view_add_heart_point(float value)
{
    s_heart_rate = value;
    if (s_chart != NULL && s_heart_series != NULL)
    {
        if (s_chart_type == LVGL_UI_CHART_HEART || s_chart_type == LVGL_UI_CHART_BOTH)
        {
            lv_chart_set_next_value(s_chart, s_heart_series, (int32_t)value);
        }
    }
}

void lvgl_radar_view_set_chart_type(lvgl_ui_chart_type_t type)
{
    s_chart_type = type;

    if (s_chart != NULL)
    {
        switch (type)
        {
        case LVGL_UI_CHART_BREATH:
            lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 40);
            if (s_heart_series != NULL)
            {
                lv_chart_hide_series(s_chart, s_heart_series, true);
            }
            if (s_breath_series != NULL)
            {
                lv_chart_hide_series(s_chart, s_breath_series, false);
            }
            break;
        case LVGL_UI_CHART_HEART:
            lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 40, 120);
            if (s_breath_series != NULL)
            {
                lv_chart_hide_series(s_chart, s_breath_series, true);
            }
            if (s_heart_series != NULL)
            {
                lv_chart_hide_series(s_chart, s_heart_series, false);
            }
            break;
        case LVGL_UI_CHART_BOTH:
            lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 120);
            if (s_breath_series != NULL)
            {
                lv_chart_hide_series(s_chart, s_breath_series, false);
            }
            if (s_heart_series != NULL)
            {
                lv_chart_hide_series(s_chart, s_heart_series, false);
            }
            break;
        }
    }
}

lvgl_ui_chart_type_t lvgl_radar_view_get_chart_type(void)
{
    return s_chart_type;
}

#endif
