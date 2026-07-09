/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "display_system.h"

#include "app_demos.h"
#include "board_config.h"
#include "board_init.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gesture_data_collector.h"
#include "gesture_recognition.h"
#include "simple_gui.h"
#include "touch_game.h"
#include "touch_gui.h"

#include <string.h>

static const char *TAG = "DISPLAY_SYSTEM";

// Display system state
static display_state_t s_state        = DISPLAY_STATE_IDLE;
static display_config_t s_config      = {0};
static display_handles_t s_handles    = {0};
static simple_gui_t s_gui             = {0};
static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_touch_handle_t s_touch = NULL;

esp_err_t display_system_init(const display_config_t *config, display_handles_t *handles)
{
    ESP_LOGI(TAG, "Initializing display system...");

    // Use default config if NULL
    if (config == NULL) {
        s_config.mode                 = DISPLAY_MODE_TOUCH_GAME;
        s_config.enable_touch         = true;
        s_config.enable_double_buffer = true;
        s_config.enable_ppa_accel     = true;
    } else {
        s_config = *config;
    }

    // Step 1: Initialize LCD hardware
    ESP_LOGI(TAG, "Initializing LCD panel...");
    board_init_lcd(&s_panel);
    s_handles.panel_handle = s_panel;
    ESP_LOGI(TAG, "LCD panel initialized");

    // Step 2: Initialize GUI context with double-buffer
    if (s_config.enable_double_buffer) {
        ESP_LOGI(TAG, "Initializing GUI with double-buffer mode...");
        gui_init_double_buffer(&s_gui, s_panel, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    } else {
        ESP_LOGI(TAG, "Initializing GUI in single-buffer mode...");
        simple_gui_init(&s_gui, s_panel, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    }
    s_handles.gui_handle = &s_gui;
    ESP_LOGI(TAG, "GUI initialized (double_buffer=%d)", s_config.enable_double_buffer);

    // Step 3: Initialize PPA hardware accelerator (if enabled)
    if (s_config.enable_ppa_accel && s_gui.ppa_fill == NULL) {
        ESP_LOGW(TAG, "PPA accelerator requested but not initialized in gui_init");
    }

    // Step 4: Initialize touch controller (if enabled)
    if (s_config.enable_touch) {
        ESP_LOGI(TAG, "Initializing touch controller...");
        esp_err_t ret = touch_init(&s_gui, &s_touch);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize touch: %s", esp_err_to_name(ret));
            // Touch init failed but display is functional - continue without touch
            s_touch                = NULL;
            s_handles.touch_handle = NULL;
            ESP_LOGW(TAG, "Display system will operate without touch input");
        } else {
            s_handles.touch_handle = s_touch;
            ESP_LOGI(TAG, "Touch controller initialized");
        }
    } else {
        ESP_LOGI(TAG, "Touch disabled by configuration");
        s_touch                = NULL;
        s_handles.touch_handle = NULL;
    }

    // Step 5: Clear screen to black
    gui_clear_screen(&s_gui, COLOR_BLACK);
    gui_draw_string(&s_gui, 10, 200, "Display System Ready", COLOR_WHITE, COLOR_BLACK, 2);
    gui_swap_buffers(&s_gui);

    s_state = DISPLAY_STATE_IDLE;
    ESP_LOGI(TAG, "Display system initialized successfully");
    ESP_LOGI(TAG, "Mode: %d, Double-buffer: %d, Touch: %d, PPA: %d", s_config.mode, s_config.enable_double_buffer,
             s_config.enable_touch, s_config.enable_ppa_accel);

    // Return handles to caller (if requested)
    if (handles != NULL) {
        *handles = s_handles;
    }

    return ESP_OK;
}

esp_err_t display_system_start(const display_handles_t *handles)
{
    ESP_LOGI(TAG, "Starting display system in mode %d...", s_config.mode);

    if (s_state != DISPLAY_STATE_IDLE) {
        ESP_LOGE(TAG, "Display system not idle (state: %d)", s_state);
        return ESP_ERR_INVALID_STATE;
    }

    // Use internal handles if NULL passed
    const display_handles_t *active_handles = (handles != NULL) ? handles : &s_handles;

    // Verify handles
    if (active_handles->panel_handle == NULL || active_handles->gui_handle == NULL) {
        ESP_LOGE(TAG, "Invalid handles: panel or GUI is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    s_state = DISPLAY_STATE_RUNNING;

    // Run the selected demo mode
    switch (s_config.mode) {
    case DISPLAY_MODE_TOUCH_GAME:
        ESP_LOGI(TAG, "Launching Touch Game demo...");
        touch_game_demo(&s_gui, s_touch);
        break;

    case DISPLAY_MODE_GESTURE_RECOGNITION:
        ESP_LOGI(TAG, "Launching Gesture Recognition demo...");
        gesture_recognition_demo(&s_gui, s_touch);
        break;

    case DISPLAY_MODE_TOUCH_GUI:
        ESP_LOGI(TAG, "Launching Touch GUI demo...");
        touch_gui_demo(&s_gui, s_touch);
        break;

    case DISPLAY_MODE_DATA_COLLECTOR:
        ESP_LOGI(TAG, "Launching Gesture Data Collector demo...");
        gesture_data_collector_demo(&s_gui, s_touch);
        break;

    default:
        ESP_LOGE(TAG, "Invalid demo mode: %d", s_config.mode);
        s_state = DISPLAY_STATE_ERROR;
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Display system started successfully");
    return ESP_OK;
}

esp_err_t display_system_stop(const display_handles_t *handles)
{
    ESP_LOGI(TAG, "Stopping display system...");

    if (s_state == DISPLAY_STATE_IDLE) {
        ESP_LOGW(TAG, "Display system already idle");
        return ESP_OK;
    }

    // Clear screen to indicate stop
    gui_clear_screen(&s_gui, COLOR_BLACK);
    gui_draw_string(&s_gui, 10, 200, "Display Stopped", COLOR_YELLOW, COLOR_BLACK, 3);
    gui_swap_buffers(&s_gui);

    s_state = DISPLAY_STATE_IDLE;
    ESP_LOGI(TAG, "Display system stopped");

    return ESP_OK;
}

display_state_t display_system_get_state(void)
{
    return s_state;
}

void display_system_deinit(display_handles_t *handles)
{
    ESP_LOGI(TAG, "Deinitializing display system...");

    // Stop if running
    if (s_state == DISPLAY_STATE_RUNNING || s_state == DISPLAY_STATE_PAUSED) {
        display_system_stop(handles);
    }

    // Step 1: Deinitialize touch controller
    if (s_touch != NULL) {
        ESP_LOGI(TAG, "Deinitializing touch controller...");
        touch_deinit(s_touch);
        s_touch                = NULL;
        s_handles.touch_handle = NULL;
        ESP_LOGI(TAG, "Touch controller deinitialized");
    }

    // Step 2: Clear GUI state
    ESP_LOGI(TAG, "Clearing GUI context...");
    memset(&s_gui, 0, sizeof(simple_gui_t));
    s_handles.gui_handle = NULL;

    // Step 3: Panel handle remains valid (MIPI DSI is global resource)
    // Note: We don't call esp_lcd_panel_del() because the MIPI DSI bus
    // is shared and should be kept alive for potential restart.
    // Only clear our reference.
    ESP_LOGI(TAG, "Releasing panel handle reference...");
    s_panel                = NULL;
    s_handles.panel_handle = NULL;

    // Step 4: Clear handles for caller
    if (handles != NULL) {
        handles->panel_handle = NULL;
        handles->gui_handle   = NULL;
        handles->touch_handle = NULL;
    }

    s_state = DISPLAY_STATE_IDLE;
    ESP_LOGI(TAG, "Display system deinitialized");
    ESP_LOGW(TAG, "Note: MIPI DSI hardware remains powered for potential restart");
}