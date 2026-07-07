/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "board_init.h"

#include "board_config.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_st7701.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "st7701s_kd034_init.h"

static const char *TAG = "BOARD_INIT";

// LDO handle kept for lifetime of program
static esp_ldo_channel_handle_t s_ldo_mipi_phy = NULL;
// I2C handles for cleanup in touch_deinit
static i2c_master_bus_handle_t s_i2c_bus_handle = NULL;
static esp_lcd_panel_io_handle_t s_io_handle    = NULL;

/* ----------------------------------------------------------------------- */
/* LCD initialization                                                       */
/* ----------------------------------------------------------------------- */

/**
 * @brief Initialize MIPI DSI PHY power via LDO regulator
 */
static void init_dsi_phy_power(void)
{
    esp_ldo_channel_config_t ldo_mipi_phy_config = {
        .chan_id    = BOARD_DSI_PHY_LDO_CHAN,
        .voltage_mv = BOARD_DSI_PHY_LDO_VOLTAGE_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_mipi_phy_config, &s_ldo_mipi_phy));
    ESP_LOGI(TAG, "MIPI DSI PHY powered on");
}

/**
 * @brief Initialize backlight GPIO
 */
static void init_backlight(void)
{
    gpio_config_t bk_gpio_config = {
        .mode         = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << BOARD_BL_GPIO,
    };
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));
    gpio_set_level(BOARD_BL_GPIO, 1);
    ESP_LOGI(TAG, "Backlight initialized and turned on");
}

void board_init_lcd(esp_lcd_panel_handle_t *out_panel)
{
    ESP_LOGI(TAG, "KD034WXFID001 (ST7701S) MIPI DSI LCD 480x480 60Hz");

    // Step 1: Enable MIPI DSI PHY power
    init_dsi_phy_power();

    // Step 2: Initialize backlight
    init_backlight();

    // Step 3: Create MIPI DSI bus
    ESP_LOGI(TAG, "Creating MIPI DSI bus...");
    esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t bus_config   = {
        .bus_id             = BOARD_DSI_BUS_ID,
        .num_data_lanes     = BOARD_DSI_LANE_NUM,
        .phy_clk_src        = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = BOARD_DSI_LANE_BITRATE_MBPS,
    };
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus));

    // Step 4: Create DBI panel IO
    ESP_LOGI(TAG, "Creating DBI panel IO...");
    esp_lcd_panel_io_handle_t mipi_dbi_io = NULL;
    esp_lcd_dbi_io_config_t dbi_config    = {
        .virtual_channel = BOARD_DSI_VIRTUAL_CHANNEL,
        .lcd_cmd_bits    = BOARD_DSI_CMD_BITS,
        .lcd_param_bits  = BOARD_DSI_PARAM_BITS,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &mipi_dbi_io));

    // Step 5: Configure DPI panel timing
    ESP_LOGI(TAG, "Configuring DPI panel timing...");
    esp_lcd_dpi_panel_config_t dpi_config = {
        .dpi_clk_src        = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = BOARD_LCD_DPI_CLK_MHZ,
        .virtual_channel    = BOARD_DSI_VIRTUAL_CHANNEL,
        .in_color_format    = BOARD_LCD_COLOR_FORMAT,
        .num_fbs            = BOARD_DPI_FB_COUNT,
        .video_timing =
            {
                .h_size            = BOARD_LCD_H_RES,
                .v_size            = BOARD_LCD_V_RES,
                .hsync_pulse_width = BOARD_LCD_HSA,
                .hsync_back_porch  = BOARD_LCD_HBP,
                .hsync_front_porch = BOARD_LCD_HFP,
                .vsync_pulse_width = BOARD_LCD_VSA,
                .vsync_back_porch  = BOARD_LCD_VBP,
                .vsync_front_porch = BOARD_LCD_VFP,
            },
    };

    // Step 6: Configure ST7701S vendor config
    st7701_vendor_config_t vendor_config = {
        .init_cmds                = kd034wxfid001_init_cmds,
        .init_cmds_size           = KD034WXFID001_INIT_CMDS_SIZE,
        .flags.use_mipi_interface = 1,
        .mipi_config =
            {
                .dsi_bus    = mipi_dsi_bus,
                .dpi_config = &dpi_config,
            },
    };

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BOARD_LCD_RESET_GPIO,
        .rgb_ele_order  = BOARD_LCD_RGB_ORDER,
        .bits_per_pixel = BOARD_LCD_BITS_PER_PIXEL,
        .vendor_config  = &vendor_config,
    };

    // Step 7: Create ST7701S panel
    ESP_LOGI(TAG, "Creating ST7701S panel...");
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7701(mipi_dbi_io, &panel_config, out_panel));

    // Step 8: Reset and initialize panel
    ESP_LOGI(TAG, "Resetting panel...");
    ESP_ERROR_CHECK(esp_lcd_panel_reset(*out_panel));

    ESP_LOGI(TAG, "Initializing panel...");
    ESP_ERROR_CHECK(esp_lcd_panel_init(*out_panel));

    ESP_LOGI(TAG, "Turning on display...");
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(*out_panel, true));

    ESP_LOGI(TAG, "Panel initialized!");
}

/* ----------------------------------------------------------------------- */
/* Touch initialization                                                     */
/* ----------------------------------------------------------------------- */

esp_err_t touch_init(simple_gui_t *gui, esp_lcd_touch_handle_t *out_tp)
{
    ESP_LOGI(TAG, "Initializing touch controller...");

    // Step 1: Create I2C master bus
    ESP_LOGI(TAG, "Creating I2C master bus...");
    i2c_master_bus_config_t i2c_bus_config = {
        .i2c_port                     = BOARD_TOUCH_I2C_PORT,
        .sda_io_num                   = BOARD_TOUCH_I2C_SDA_GPIO,
        .scl_io_num                   = BOARD_TOUCH_I2C_SCL_GPIO,
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt            = BOARD_TOUCH_I2C_GLITCH_CNT,
        .flags.enable_internal_pullup = BOARD_TOUCH_I2C_PULLUP,
    };

    esp_err_t ret = i2c_new_master_bus(&i2c_bus_config, &s_i2c_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2C master bus: %s", esp_err_to_name(ret));
        gui_clear_screen(gui, COLOR_RED);
        gui_draw_string(gui, 10, 200, "I2C BUS FAIL", COLOR_WHITE, COLOR_RED, 4);
        gui_swap_buffers(gui);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return ret;
    }

    // Step 2: Create esp_lcd_panel_io_i2c
    ESP_LOGI(TAG, "Creating LCD panel IO I2C...");
    esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();

    ret = esp_lcd_new_panel_io_i2c(s_i2c_bus_handle, &io_config, &s_io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create panel IO I2C: %s", esp_err_to_name(ret));
        gui_clear_screen(gui, COLOR_RED);
        gui_draw_string(gui, 10, 200, "IO I2C FAIL", COLOR_WHITE, COLOR_RED, 4);
        gui_swap_buffers(gui);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return ret;
    }

    // Step 3: Initialize GT911 touch controller
    ESP_LOGI(TAG, "Initializing GT911 touch controller...");
    esp_lcd_touch_io_gt911_config_t tp_gt911_config = {
        .dev_addr = io_config.dev_addr,
    };

    esp_lcd_touch_config_t tp_cfg = {
        .x_max        = BOARD_LCD_H_RES,
        .y_max        = BOARD_LCD_V_RES,
        .rst_gpio_num = BOARD_TOUCH_RST_GPIO,
        .int_gpio_num = BOARD_TOUCH_INT_GPIO,
        .levels =
            {
                .reset     = BOARD_TOUCH_RST_ACTIVE_LOW,
                .interrupt = BOARD_TOUCH_INT_ACTIVE_LOW,
            },
        .flags =
            {
                .swap_xy  = BOARD_TOUCH_SWAP_XY,
                .mirror_x = BOARD_TOUCH_MIRROR_X,
                .mirror_y = BOARD_TOUCH_MIRROR_Y,
            },
        .driver_data = &tp_gt911_config,
    };

    ret = esp_lcd_touch_new_i2c_gt911(s_io_handle, &tp_cfg, out_tp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GT911 initialization failed: %s", esp_err_to_name(ret));
        gui_clear_screen(gui, COLOR_RED);
        gui_draw_string(gui, 10, 200, "GT911 INIT FAIL", COLOR_WHITE, COLOR_RED, 4);
        gui_swap_buffers(gui);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return ret;
    }

    ESP_LOGI(TAG, "GT911 initialized successfully");
    return ESP_OK;
}

void touch_deinit(esp_lcd_touch_handle_t tp)
{
    if (tp) {
        esp_lcd_touch_del(tp);
    }
    if (s_io_handle) {
        esp_lcd_panel_io_del(s_io_handle);
        s_io_handle = NULL;
    }
    if (s_i2c_bus_handle) {
        i2c_del_master_bus(s_i2c_bus_handle);
        s_i2c_bus_handle = NULL;
    }
    ESP_LOGI(TAG, "Touch deinitialized");
}
