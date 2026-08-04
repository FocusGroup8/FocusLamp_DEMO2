/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_lcd_gc9107.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lcd_panel.gc9107";

/* GC9107 commands */
#define GC9107_CMD_SWRESET   0x01
#define GC9107_CMD_SLPOUT    0x11
#define GC9107_CMD_DISPOFF   0x28
#define GC9107_CMD_DISPON    0x29
#define GC9107_CMD_CASET     0x2A
#define GC9107_CMD_RASET     0x2B
#define GC9107_CMD_RAMWR     0x2C
#define GC9107_CMD_MADCTL    0x36
#define GC9107_CMD_COLMOD    0x3A

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    uint8_t madctl;
    uint8_t bits_per_pixel;
    int x_gap;
    int y_gap;
} gc9107_panel_t;

static esp_err_t panel_gc9107_del(esp_lcd_panel_t *panel)
{
    gc9107_panel_t *gc9107 = __containerof(panel, gc9107_panel_t, base);
    if (gc9107->reset_gpio_num >= 0) {
        gpio_reset_pin(gc9107->reset_gpio_num);
    }
    ESP_LOGD(TAG, "del gc9107 panel @%p", gc9107);
    free(gc9107);
    return ESP_OK;
}

static esp_err_t panel_gc9107_reset(esp_lcd_panel_t *panel)
{
    gc9107_panel_t *gc9107 = __containerof(panel, gc9107_panel_t, base);
    esp_lcd_panel_io_handle_t io = gc9107->io;

    /* Perform hardware reset */
    if (gc9107->reset_gpio_num >= 0) {
        gpio_set_level(gc9107->reset_gpio_num, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(gc9107->reset_gpio_num, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(gc9107->reset_gpio_num, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
    } else {
        /* Software reset */
        esp_lcd_panel_io_tx_param(io, GC9107_CMD_SWRESET, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    return ESP_OK;
}

static esp_err_t panel_gc9107_init(esp_lcd_panel_t *panel)
{
    gc9107_panel_t *gc9107 = __containerof(panel, gc9107_panel_t, base);
    esp_lcd_panel_io_handle_t io = gc9107->io;

    /* GC9107 init sequence for 1.45" TFT (vendor reference) */
    ESP_LOGI(TAG, "GC9107 init start (MADCTL=0x%02X)", gc9107->madctl);

    /* Page unlock */
    esp_lcd_panel_io_tx_param(io, 0xFE, NULL, 0);
    esp_lcd_panel_io_tx_param(io, 0xEF, NULL, 0);

    /* Power settings */
    uint8_t b0 = 0xC0;  esp_lcd_panel_io_tx_param(io, 0xB0, &b0, 1);
    uint8_t b2 = 0x2F;  esp_lcd_panel_io_tx_param(io, 0xB2, &b2, 1);
    uint8_t b3 = 0x03;  esp_lcd_panel_io_tx_param(io, 0xB3, &b3, 1);
    uint8_t b6 = 0x19;  esp_lcd_panel_io_tx_param(io, 0xB6, &b6, 1);
    uint8_t b7 = 0x01;  esp_lcd_panel_io_tx_param(io, 0xB7, &b7, 1);
    uint8_t ac = 0xCB;  esp_lcd_panel_io_tx_param(io, 0xAC, &ac, 1);
    uint8_t ab = 0x07;  esp_lcd_panel_io_tx_param(io, 0xAB, &ab, 1);
    uint8_t b4 = 0x00;  esp_lcd_panel_io_tx_param(io, 0xB4, &b4, 1);
    uint8_t a8 = 0x0C;  esp_lcd_panel_io_tx_param(io, 0xA8, &a8, 1);

    /* COLMOD: RGB565 */
    uint8_t colmod = 0x05;
    esp_lcd_panel_io_tx_param(io, GC9107_CMD_COLMOD, &colmod, 1);

    /* MADCTL: orientation */
    esp_lcd_panel_io_tx_param(io, GC9107_CMD_MADCTL, &gc9107->madctl, 1);

    /* Display mode */
    uint8_t b8 = 0x08;  esp_lcd_panel_io_tx_param(io, 0xB8, &b8, 1);

    /* Booster settings */
    uint8_t e8 = 0x23;  esp_lcd_panel_io_tx_param(io, 0xE8, &e8, 1);
    uint8_t e9 = 0x47;  esp_lcd_panel_io_tx_param(io, 0xE9, &e9, 1);
    uint8_t ea = 0x44;  esp_lcd_panel_io_tx_param(io, 0xEA, &ea, 1);
    uint8_t eb = 0xE0;  esp_lcd_panel_io_tx_param(io, 0xEB, &eb, 1);
    uint8_t ed = 0x03;  esp_lcd_panel_io_tx_param(io, 0xED, &ed, 1);
    uint8_t c6 = 0x19;  esp_lcd_panel_io_tx_param(io, 0xC6, &c6, 1);
    uint8_t c7 = 0x10;  esp_lcd_panel_io_tx_param(io, 0xC7, &c7, 1);

    /* Gamma settings */
    uint8_t gamma_pos[] = {0x0B, 0x2F, 0x10, 0x4B, 0x28, 0x3F, 0x3E,
                           0x60, 0x00, 0x12, 0x12, 0x0F, 0x00, 0x1F};
    esp_lcd_panel_io_tx_param(io, 0xF0, gamma_pos, sizeof(gamma_pos));

    uint8_t gamma_neg[] = {0x0E, 0x3D, 0x2A, 0x40, 0xFA, 0x00, 0x02,
                           0x60, 0x00, 0x03, 0x13, 0x00, 0x10, 0x1F};
    esp_lcd_panel_io_tx_param(io, 0xF1, gamma_neg, sizeof(gamma_neg));

    /* SLPOUT */
    esp_lcd_panel_io_tx_param(io, GC9107_CMD_SLPOUT, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));

    /* DISPON */
    esp_lcd_panel_io_tx_param(io, GC9107_CMD_DISPON, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));

    ESP_LOGI(TAG, "GC9107 init complete");
    return ESP_OK;
}

static esp_err_t panel_gc9107_draw_bitmap(esp_lcd_panel_t *panel,
                                          int x_start, int y_start,
                                          int x_end, int y_end,
                                          const void *color_data)
{
    gc9107_panel_t *gc9107 = __containerof(panel, gc9107_panel_t, base);
    esp_lcd_panel_io_handle_t io = gc9107->io;

    /* Apply offsets */
    x_start += gc9107->x_gap;
    x_end += gc9107->x_gap;
    y_start += gc9107->y_gap;
    y_end += gc9107->y_gap;

    /* Column address set */
    uint8_t col_data[4] = {
        (x_start >> 8) & 0xFF, x_start & 0xFF,
        ((x_end - 1) >> 8) & 0xFF, (x_end - 1) & 0xFF
    };
    esp_lcd_panel_io_tx_param(io, GC9107_CMD_CASET, col_data, 4);

    /* Row address set */
    uint8_t row_data[4] = {
        (y_start >> 8) & 0xFF, y_start & 0xFF,
        ((y_end - 1) >> 8) & 0xFF, (y_end - 1) & 0xFF
    };
    esp_lcd_panel_io_tx_param(io, GC9107_CMD_RASET, row_data, 4);

    /* Memory write */
    size_t len = (x_end - x_start) * (y_end - y_start) * gc9107->bits_per_pixel / 8;
    esp_lcd_panel_io_tx_color(io, GC9107_CMD_RAMWR, color_data, len);

    return ESP_OK;
}

static esp_err_t panel_gc9107_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    gc9107_panel_t *gc9107 = __containerof(panel, gc9107_panel_t, base);
    esp_lcd_panel_io_handle_t io = gc9107->io;
    /* GC9107: 0x20 = normal, 0x21 = inverted */
    uint8_t cmd = invert_color_data ? 0x21 : 0x20;
    esp_lcd_panel_io_tx_param(io, cmd, NULL, 0);
    return ESP_OK;
}

static esp_err_t panel_gc9107_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    gc9107_panel_t *gc9107 = __containerof(panel, gc9107_panel_t, base);
    esp_lcd_panel_io_handle_t io = gc9107->io;

    uint8_t madctl = gc9107->madctl;
    if (mirror_x) madctl |= 0x40;   /* MX */
    if (mirror_y) madctl |= 0x80;   /* MY */
    gc9107->madctl = madctl;
    esp_lcd_panel_io_tx_param(io, GC9107_CMD_MADCTL, &madctl, 1);
    return ESP_OK;
}

static esp_err_t panel_gc9107_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    gc9107_panel_t *gc9107 = __containerof(panel, gc9107_panel_t, base);
    esp_lcd_panel_io_handle_t io = gc9107->io;

    uint8_t madctl = gc9107->madctl;
    if (swap_axes) {
        madctl |= 0x20;   /* MV */
    } else {
        madctl &= ~0x20;
    }
    gc9107->madctl = madctl;
    esp_lcd_panel_io_tx_param(io, GC9107_CMD_MADCTL, &madctl, 1);
    return ESP_OK;
}

static esp_err_t panel_gc9107_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    gc9107_panel_t *gc9107 = __containerof(panel, gc9107_panel_t, base);
    gc9107->x_gap = x_gap;
    gc9107->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_gc9107_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    gc9107_panel_t *gc9107 = __containerof(panel, gc9107_panel_t, base);
    esp_lcd_panel_io_handle_t io = gc9107->io;
    uint8_t cmd = on_off ? GC9107_CMD_DISPON : GC9107_CMD_DISPOFF;
    esp_lcd_panel_io_tx_param(io, cmd, NULL, 0);
    return ESP_OK;
}

esp_err_t esp_lcd_new_panel_gc9107(esp_lcd_panel_io_handle_t io,
                                   const esp_lcd_panel_dev_config_t *panel_dev,
                                   esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t ret = ESP_OK;
    gc9107_panel_t *gc9107 = NULL;
    ESP_GOTO_ON_FALSE(io && panel_dev && ret_panel, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");
    ESP_GOTO_ON_FALSE(panel_dev->bits_per_pixel == 16, ESP_ERR_INVALID_ARG, err, TAG,
                      "bits per pixel must be 16");

    gc9107 = calloc(1, sizeof(gc9107_panel_t));
    ESP_GOTO_ON_FALSE(gc9107, ESP_ERR_NO_MEM, err, TAG, "no mem for gc9107 panel");

    gc9107->io = io;
    gc9107->bits_per_pixel = panel_dev->bits_per_pixel;
    gc9107->reset_gpio_num = panel_dev->reset_gpio_num;

    /* Default MADCTL: portrait, RGB order from config */
    uint8_t madctl = 0x00;
    if (panel_dev->rgb_ele_order == LCD_RGB_ELEMENT_ORDER_BGR) {
        madctl |= 0x08;  /* BGR bit */
    }
    gc9107->madctl = madctl;

    /* Default gaps (no offset) - set by user via esp_lcd_panel_set_gap */
    gc9107->x_gap = 0;
    gc9107->y_gap = 0;

    /* Configure reset GPIO if specified */
    if (gc9107->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << gc9107->reset_gpio_num,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "config reset gpio failed");
    }

    /* Register panel callbacks */
    gc9107->base.del = panel_gc9107_del;
    gc9107->base.reset = panel_gc9107_reset;
    gc9107->base.init = panel_gc9107_init;
    gc9107->base.draw_bitmap = panel_gc9107_draw_bitmap;
    gc9107->base.invert_color = panel_gc9107_invert_color;
    gc9107->base.mirror = panel_gc9107_mirror;
    gc9107->base.swap_xy = panel_gc9107_swap_xy;
    gc9107->base.set_gap = panel_gc9107_set_gap;
    gc9107->base.disp_on_off = panel_gc9107_disp_on_off;

    *ret_panel = &gc9107->base;
    ESP_LOGD(TAG, "new gc9107 panel @%p", gc9107);
    return ESP_OK;

err:
    if (gc9107) {
        free(gc9107);
    }
    return ret;
}
