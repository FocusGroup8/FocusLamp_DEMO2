/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lvgl_display.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include <string.h>

static const char *TAG = "LVGL_DISP";

esp_err_t lvgl_display_init(const lvgl_display_cfg_t *cfg, lvgl_display_t *ctx)
{
    if (!cfg || !ctx) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(ctx, 0, sizeof(lvgl_display_t));
    ctx->panel = cfg->panel_handle;

    /* Step 1: Initialize esp_lvgl_port (LVGL core + task + timer) */
    ESP_LOGI(TAG, "Initializing LVGL port...");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority   = 4;
    port_cfg.task_stack      = 7168;
    port_cfg.task_affinity   = -1;
    port_cfg.timer_period_ms = 5;

    esp_err_t ret = lvgl_port_init(&port_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init LVGL port: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "LVGL port initialized");

    /* Step 2: Add MIPI-DSI display */
    ESP_LOGI(TAG, "Adding MIPI-DSI display (%lux%lu, avoid_tearing=%d)...", (unsigned long)cfg->hres,
             (unsigned long)cfg->vres, cfg->avoid_tearing);

    lvgl_port_display_cfg_t disp_cfg = {
        .panel_handle  = cfg->panel_handle,
        .buffer_size   = cfg->hres * cfg->vres / 10, /* 1/10 screen for partial mode */
        .double_buffer = true,
        .hres          = cfg->hres,
        .vres          = cfg->vres,
        .color_format  = LV_COLOR_FORMAT_RGB565,
        .flags =
            {
                .buff_dma    = 0,
                .buff_spiram = 1,
                .swap_bytes  = 0, /* DPI panel expects little-endian RGB565 (BSP_LCD_BIGENDIAN=0) */
            },
    };

    lvgl_port_display_dsi_cfg_t dsi_cfg = {
        .flags =
            {
                .avoid_tearing = cfg->avoid_tearing,
            },
    };

    ctx->disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    if (ctx->disp == NULL) {
        ESP_LOGE(TAG, "Failed to add DSI display");
        lvgl_port_deinit();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "DSI display added successfully");

    /* Step 3: Add touch input (if available) */
    if (cfg->touch_handle != NULL) {
        ESP_LOGI(TAG, "Adding touch input...");
        lvgl_port_touch_cfg_t touch_cfg = {
            .disp   = ctx->disp,
            .handle = cfg->touch_handle,
            .scale  = {1.0f, 1.0f},
        };
        ctx->touch_indev = lvgl_port_add_touch(&touch_cfg);
        if (ctx->touch_indev == NULL) {
            ESP_LOGW(TAG, "Failed to add touch input, continuing without touch");
        } else {
            ESP_LOGI(TAG, "Touch input added");
        }
    }

    ESP_LOGI(TAG, "LVGL display system initialized");
    return ESP_OK;
}

void lvgl_display_demo_ui(lvgl_display_t *ctx)
{
    if (!ctx || !ctx->disp) {
        return;
    }

    lvgl_port_lock(0);

    /* Create a screen with title label */
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);

    /* Title label */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "ESP32-P4 LVGL");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    /* Subtitle */
    lv_obj_t *subtitle = lv_label_create(scr);
    lv_label_set_text(subtitle, "MIPI DSI 480x480 RGB888");
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 60);

    /* Test button */
    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, 160, 50);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 40);
    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "LVGL Ready");
    lv_obj_center(btn_label);

    /* Status label at bottom */
    lv_obj_t *status = lv_label_create(scr);
    lv_label_set_text(status, "Display system running...");
    lv_obj_set_style_text_color(status, lv_color_hex(0x00FF00), 0);
    lv_obj_align(status, LV_ALIGN_BOTTOM_MID, 0, -20);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "Demo UI created");
}

esp_err_t lvgl_display_deinit(lvgl_display_t *ctx)
{
    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Deinitializing LVGL display...");

    /* Remove touch input */
    if (ctx->touch_indev != NULL) {
        lvgl_port_remove_touch(ctx->touch_indev);
        ctx->touch_indev = NULL;
    }

    /* Remove display */
    if (ctx->disp != NULL) {
        lvgl_port_remove_disp(ctx->disp);
        ctx->disp = NULL;
    }

    /* Deinitialize LVGL port */
    esp_err_t ret = lvgl_port_deinit();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "LVGL port deinit failed: %s", esp_err_to_name(ret));
    }

    ctx->canvas = NULL;
    memset(ctx, 0, sizeof(lvgl_display_t));
    ESP_LOGI(TAG, "LVGL display deinitialized");
    return ESP_OK;
}

esp_err_t lvgl_display_create_canvas(lvgl_display_t *ctx, uint8_t *buf, uint32_t width, uint32_t height)
{
    if (!ctx || !ctx->disp || !buf) {
        return ESP_ERR_INVALID_ARG;
    }

    lvgl_port_lock(0);

    /* Clear existing screen content */
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);

    /* Create canvas covering the full screen
     * Use RGB565 (little-endian) format — PPA byte_swap=true converts
     * ISP's big-endian output to little-endian, matching the DPI panel
     * (BSP_LCD_BIGENDIAN=0) and LVGL's native RGB565 format.
     */
    ctx->canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(ctx->canvas, buf, width, height, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(ctx->canvas);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "Canvas created (%lux%lu RGB565, buf=%p)", (unsigned long)width, (unsigned long)height, buf);
    return ESP_OK;
}

void lvgl_display_update_canvas(lvgl_display_t *ctx, uint8_t *buf, uint32_t width, uint32_t height)
{
    if (!ctx || !ctx->canvas) {
        return;
    }

    /* Caller must hold LVGL lock — this matches the official ESP-BSP
     * display_camera_video pattern where lock is managed externally. */
    lv_canvas_set_buffer(ctx->canvas, buf, width, height, LV_COLOR_FORMAT_RGB565);
    lv_obj_invalidate(ctx->canvas);
}
