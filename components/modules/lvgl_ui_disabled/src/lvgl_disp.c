#include "lvgl_disp.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lcd_driver.h"
#include "lvgl_ui_config.h"

#if (LVGL_UI_ENABLE == 1)

static const char* TAG = "LVGL_DISP";

static lv_display_t*  s_disp = NULL;
static lv_draw_buf_t* s_buf1 = NULL;
#if (LVGL_UI_DOUBLE_BUFFER == 1)
static lv_draw_buf_t* s_buf2 = NULL;
#endif

static uint8_t s_spi_buf[LVGL_UI_LCD_WIDTH * LVGL_UI_LCD_HEIGHT * 2];

static void lvgl_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map)
{
    (void)disp;

    int32_t w           = lv_area_get_width(area);
    int32_t h           = lv_area_get_height(area);
    size_t  pixel_count = w * h;

    if (pixel_count > 0 && px_map != NULL)
    {
        uint16_t* px16 = (uint16_t*)px_map;

        for (size_t i = 0; i < pixel_count; i++)
        {
            uint16_t color       = px16[i];
            s_spi_buf[i * 2]     = (color >> 8) & 0xFF;
            s_spi_buf[i * 2 + 1] = color & 0xFF;
        }

        lcd_driver_flush_area(area->x1, area->y1, area->x2, area->y2, s_spi_buf, pixel_count * 2);
    }

    lv_display_flush_ready(disp);
}

esp_err_t lvgl_disp_init(void)
{
    if (s_disp != NULL)
    {
        ESP_LOGW(TAG, "LVGL display already initialized");
        return ESP_OK;
    }

    uint32_t buf_size =
        (LVGL_UI_LCD_WIDTH * LVGL_UI_LCD_HEIGHT * LVGL_UI_BUFFER_SIZE_PERCENT) / 100;
    if (buf_size < 10)
    {
        buf_size = 10;
    }

    s_disp = lv_display_create(LVGL_UI_LCD_WIDTH, LVGL_UI_LCD_HEIGHT);
    if (s_disp == NULL)
    {
        ESP_LOGE(TAG, "Failed to create LVGL display");
        return ESP_FAIL;
    }

    s_buf1 = lv_draw_buf_create(LVGL_UI_LCD_WIDTH, buf_size / LVGL_UI_LCD_WIDTH,
                                LV_COLOR_FORMAT_RGB565, 0);
    if (s_buf1 == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate LVGL draw buffer 1");
        lv_display_delete(s_disp);
        s_disp = NULL;
        return ESP_ERR_NO_MEM;
    }

#if (LVGL_UI_DOUBLE_BUFFER == 1)
    s_buf2 = lv_draw_buf_create(LVGL_UI_LCD_WIDTH, buf_size / LVGL_UI_LCD_WIDTH,
                                LV_COLOR_FORMAT_RGB565, 0);
    if (s_buf2 == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate LVGL draw buffer 2");
        lv_draw_buf_destroy(s_buf1);
        s_buf1 = NULL;
        lv_display_delete(s_disp);
        s_disp = NULL;
        return ESP_ERR_NO_MEM;
    }
#endif

    lv_display_set_flush_cb(s_disp, lvgl_flush_cb);
#if (LVGL_UI_DOUBLE_BUFFER == 1)
    lv_display_set_draw_buffers(s_disp, s_buf1, s_buf2);
#else
    lv_display_set_draw_buffers(s_disp, s_buf1, NULL);
#endif
    lv_display_set_render_mode(s_disp, LV_DISPLAY_RENDER_MODE_PARTIAL);

    ESP_LOGI(TAG, "LVGL display initialized: %dx%d, buffer=%lu pixels, double=%d",
             LVGL_UI_LCD_WIDTH, LVGL_UI_LCD_HEIGHT, buf_size, LVGL_UI_DOUBLE_BUFFER);
    return ESP_OK;
}

esp_err_t lvgl_disp_deinit(void)
{
    if (s_disp == NULL)
    {
        ESP_LOGW(TAG, "LVGL display not initialized");
        return ESP_OK;
    }

    lv_display_delete(s_disp);
    s_disp = NULL;

    if (s_buf1 != NULL)
    {
        lv_draw_buf_destroy(s_buf1);
        s_buf1 = NULL;
    }
#if (LVGL_UI_DOUBLE_BUFFER == 1)
    if (s_buf2 != NULL)
    {
        lv_draw_buf_destroy(s_buf2);
        s_buf2 = NULL;
    }
#endif

    ESP_LOGI(TAG, "LVGL display deinitialized");
    return ESP_OK;
}

lv_display_t* lvgl_disp_get(void)
{
    return s_disp;
}

#endif
