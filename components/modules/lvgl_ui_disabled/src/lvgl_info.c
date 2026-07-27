#include "lvgl_info.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "lvgl_ui_config.h"

#if (LVGL_UI_ENABLE == 1) && (LVGL_UI_ENABLE_INFO == 1)

static const char* TAG = "LVGL_INFO";

static lv_obj_t* s_screen       = NULL;
static lv_obj_t* s_status_label = NULL;
static lv_obj_t* s_title_label  = NULL;
static lv_obj_t* s_value_label  = NULL;
static lv_obj_t* s_progress_bar = NULL;
static lv_obj_t* s_extra_label  = NULL;
static lv_obj_t* s_time_label   = NULL;

static char s_title[32]    = "Info";
static int  s_value        = 0;
static int  s_progress     = 0;
static bool s_status       = false;
static int  s_extra_value  = 0;
static int  s_time_seconds = 0;

static void update_display(void)
{
    if (s_status_label != NULL)
    {
        lv_label_set_text(s_status_label, s_status ? "*" : "-");
    }

    if (s_title_label != NULL)
    {
        lv_label_set_text(s_title_label, s_title);
    }

    if (s_value_label != NULL)
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", s_value);
        lv_label_set_text(s_value_label, buf);
    }

    if (s_progress_bar != NULL)
    {
        lv_bar_set_value(s_progress_bar, s_progress, LV_ANIM_OFF);
    }

    if (s_extra_label != NULL)
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "E:%d", s_extra_value);
        lv_label_set_text(s_extra_label, buf);
    }

    if (s_time_label != NULL)
    {
        int  minutes = s_time_seconds / 60;
        int  seconds = s_time_seconds % 60;
        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d", minutes, seconds);
        lv_label_set_text(s_time_label, buf);
    }
}

esp_err_t lvgl_info_init(lv_obj_t* parent)
{
    if (s_screen != NULL)
    {
        ESP_LOGW(TAG, "Info page already initialized");
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

    ESP_LOGI(TAG, "Creating status label...");
    s_status_label = lv_label_create(s_screen);
    if (s_status_label == NULL)
    {
        ESP_LOGE(TAG, "Failed to create status label");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Status label created: %p", s_status_label);
    lv_label_set_text(s_status_label, "-");
    lv_obj_set_pos(s_status_label, 2, 2);
    lv_obj_set_style_bg_opa(s_status_label, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_make(0x00, 0xFF, 0xFF), 0);

    ESP_LOGI(TAG, "Creating title label...");
    s_title_label = lv_label_create(s_screen);
    if (s_title_label == NULL)
    {
        ESP_LOGE(TAG, "Failed to create title label");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Title label created: %p", s_title_label);
    lv_label_set_text(s_title_label, s_title);
    lv_obj_set_pos(s_title_label, 22, 2);
    lv_obj_set_style_bg_opa(s_title_label, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(s_title_label, lv_color_make(0x00, 0xFF, 0xFF), 0);

    ESP_LOGI(TAG, "Creating value label...");
    s_value_label = lv_label_create(s_screen);
    if (s_value_label == NULL)
    {
        ESP_LOGE(TAG, "Failed to create value label");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Value label created: %p", s_value_label);
    lv_label_set_text(s_value_label, "0");
    lv_obj_set_pos(s_value_label, 110, 2);
    lv_obj_set_style_bg_opa(s_value_label, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(s_value_label, lv_color_make(0x00, 0xFF, 0xFF), 0);

    ESP_LOGI(TAG, "Creating progress bar...");
    s_progress_bar = lv_bar_create(s_screen);
    if (s_progress_bar == NULL)
    {
        ESP_LOGE(TAG, "Failed to create progress bar");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Progress bar created: %p", s_progress_bar);
    lv_obj_set_pos(s_progress_bar, 2, 25);
    lv_obj_set_size(s_progress_bar, 156, 6);
    lv_bar_set_range(s_progress_bar, 0, 100);
    lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_progress_bar, lv_color_make(0x20, 0x20, 0x20), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_progress_bar, lv_color_make(0x00, 0xFF, 0xFF), LV_PART_INDICATOR);

    ESP_LOGI(TAG, "Creating extra label...");
    s_extra_label = lv_label_create(s_screen);
    if (s_extra_label == NULL)
    {
        ESP_LOGE(TAG, "Failed to create extra label");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Extra label created: %p", s_extra_label);
    lv_label_set_text(s_extra_label, "E:0");
    lv_obj_set_pos(s_extra_label, 2, 48);
    lv_obj_set_style_bg_opa(s_extra_label, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(s_extra_label, lv_color_make(0x00, 0xFF, 0xFF), 0);

    ESP_LOGI(TAG, "Creating time label...");
    s_time_label = lv_label_create(s_screen);
    if (s_time_label == NULL)
    {
        ESP_LOGE(TAG, "Failed to create time label");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Time label created: %p", s_time_label);
    lv_label_set_text(s_time_label, "00:00");
    lv_obj_set_pos(s_time_label, 110, 48);
    lv_obj_set_style_bg_opa(s_time_label, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(s_time_label, lv_color_make(0x00, 0xFF, 0xFF), 0);

    ESP_LOGI(TAG, "Info page initialized");
    return ESP_OK;
}

void lvgl_info_deinit(void)
{
    if (s_time_label != NULL)
    {
        lv_obj_delete_async(s_time_label);
        s_time_label = NULL;
    }

    if (s_extra_label != NULL)
    {
        lv_obj_delete_async(s_extra_label);
        s_extra_label = NULL;
    }

    if (s_progress_bar != NULL)
    {
        lv_obj_delete_async(s_progress_bar);
        s_progress_bar = NULL;
    }

    if (s_value_label != NULL)
    {
        lv_obj_delete_async(s_value_label);
        s_value_label = NULL;
    }

    if (s_title_label != NULL)
    {
        lv_obj_delete_async(s_title_label);
        s_title_label = NULL;
    }

    if (s_status_label != NULL)
    {
        lv_obj_delete_async(s_status_label);
        s_status_label = NULL;
    }

    s_screen = NULL;

    ESP_LOGI(TAG, "Info page deinitialized");
}

void lvgl_info_update(void)
{
    update_display();
}

esp_err_t lvgl_info_set_title(const char* title)
{
    if (title == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(s_title, title, sizeof(s_title) - 1);
    s_title[sizeof(s_title) - 1] = '\0';
    return ESP_OK;
}

esp_err_t lvgl_info_set_value(int value)
{
    s_value = value;
    return ESP_OK;
}

esp_err_t lvgl_info_set_progress(int progress)
{
    if (progress < 0)
    {
        progress = 0;
    }
    else if (progress > 100)
    {
        progress = 100;
    }
    s_progress = progress;
    return ESP_OK;
}

esp_err_t lvgl_info_set_status(bool status)
{
    s_status = status;
    return ESP_OK;
}

esp_err_t lvgl_info_set_extra_value(int value)
{
    s_extra_value = value;
    return ESP_OK;
}

esp_err_t lvgl_info_set_time(int seconds)
{
    if (seconds < 0)
    {
        seconds = 0;
    }
    s_time_seconds = seconds;
    return ESP_OK;
}

#endif
