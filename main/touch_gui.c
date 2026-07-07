/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "touch_gui.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "touch_gui";

void touch_button_init(touch_button_t *button, int x, int y, int width, int height, const char *text,
                       button_callback_t callback, void *user_data)
{
    button->x               = x;
    button->y               = y;
    button->width           = width;
    button->height          = height;
    button->text            = text;
    button->text_size       = 2;
    button->color_idle      = COLOR_BLUE;
    button->color_pressed   = COLOR_CYAN;
    button->text_color      = COLOR_WHITE;
    button->state           = BUTTON_STATE_IDLE;
    button->callback        = callback;
    button->user_data       = user_data;
    button->last_press_time = 0;
    button->debounce_ms     = 200; // Default 200ms debounce interval
}

void touch_button_draw(simple_gui_t *gui, touch_button_t *button)
{
    uint32_t bg_color = (button->state == BUTTON_STATE_PRESSED) ? button->color_pressed : button->color_idle;

    // Draw button background
    gui_draw_filled_rect(gui, button->x, button->y, button->x + button->width - 1, button->y + button->height - 1,
                         bg_color);

    // Draw button border
    gui_draw_rect_outline(gui, button->x, button->y, button->x + button->width - 1, button->y + button->height - 1,
                          COLOR_WHITE, 2);

    // Draw button text (centered)
    if (button->text) {
        int text_len    = strlen(button->text);
        int char_width  = 8 * button->text_size;
        int text_width  = text_len * char_width;
        int text_height = 8 * button->text_size;

        int text_x = button->x + (button->width - text_width) / 2;
        int text_y = button->y + (button->height - text_height) / 2;

        gui_draw_string(gui, text_x, text_y, button->text, button->text_color, bg_color, button->text_size);
    }
}

bool touch_button_check_hit(touch_button_t *button, uint16_t touch_x, uint16_t touch_y)
{
    return (touch_x >= button->x && touch_x < button->x + button->width && touch_y >= button->y &&
            touch_y < button->y + button->height);
}

bool touch_button_update(simple_gui_t *gui, touch_button_t *button, uint16_t touch_x, uint16_t touch_y,
                         bool touch_active)
{
    bool state_changed    = false;
    uint32_t current_time = (uint32_t)(esp_timer_get_time() / 1000); // Get time in milliseconds

    if (touch_active && touch_button_check_hit(button, touch_x, touch_y)) {
        // Touch is active and inside button
        if (button->state == BUTTON_STATE_IDLE) {
            // Check debounce: only trigger if enough time has passed since last press
            if (current_time - button->last_press_time >= button->debounce_ms) {
                button->state           = BUTTON_STATE_PRESSED;
                button->last_press_time = current_time;
                state_changed           = true;
                ESP_LOGI(TAG, "Button '%s' pressed at (%d, %d)", button->text ? button->text : "unnamed", touch_x,
                         touch_y);
            }
        }
    } else if (!touch_active) {
        // Touch is not active (released anywhere)
        if (button->state == BUTTON_STATE_PRESSED) {
            button->state = BUTTON_STATE_RELEASED;
            state_changed = true;

            // Trigger callback on release (button was pressed before)
            if (button->callback) {
                button->callback(button->user_data);
            }

            // Return to idle state after handling release
            button->state = BUTTON_STATE_IDLE;
        }
    }

    // Redraw button if state changed
    if (state_changed) {
        touch_button_draw(gui, button);
    }

    return state_changed;
}

// Demo callback functions
static int button_click_count[4] = {0, 0, 0, 0};

static void button_callback_0(void *user_data)
{
    button_click_count[0]++;
    ESP_LOGI(TAG, "Button 1 clicked! Count: %d", button_click_count[0]);
}

static void button_callback_1(void *user_data)
{
    button_click_count[1]++;
    ESP_LOGI(TAG, "Button 2 clicked! Count: %d", button_click_count[1]);
}

static void button_callback_2(void *user_data)
{
    button_click_count[2]++;
    ESP_LOGI(TAG, "Button 3 clicked! Count: %d", button_click_count[2]);
}

static void button_callback_3(void *user_data)
{
    button_click_count[3]++;
    ESP_LOGI(TAG, "Button 4 clicked! Count: %d", button_click_count[3]);
}

void touch_gui_demo(simple_gui_t *gui, esp_lcd_touch_handle_t tp)
{
    ESP_LOGI(TAG, "Starting Touch GUI Demo...");

    // Clear screen
    gui_clear_screen(gui, COLOR_BLACK);

    // Draw title
    gui_draw_string(gui, 10, 10, "TOUCH GUI DEMO", COLOR_WHITE, COLOR_BLACK, 3);
    gui_draw_string(gui, 10, 50, "Tap buttons to interact", COLOR_GREEN, COLOR_BLACK, 2);

    // Create 4 touch buttons in a grid layout
    touch_button_t buttons[4];

    // Button 1 (Top-left)
    touch_button_init(&buttons[0], 20, 100, 200, 80, "Button 1", button_callback_0, NULL);
    buttons[0].color_idle    = COLOR_RED;
    buttons[0].color_pressed = COLOR_ORANGE;

    // Button 2 (Top-right)
    touch_button_init(&buttons[1], 260, 100, 200, 80, "Button 2", button_callback_1, NULL);
    buttons[1].color_idle    = COLOR_GREEN;
    buttons[1].color_pressed = COLOR_YELLOW;

    // Button 3 (Bottom-left)
    touch_button_init(&buttons[2], 20, 200, 200, 80, "Button 3", button_callback_2, NULL);
    buttons[2].color_idle    = COLOR_BLUE;
    buttons[2].color_pressed = COLOR_CYAN;

    // Button 4 (Bottom-right)
    touch_button_init(&buttons[3], 260, 200, 200, 80, "Button 4", button_callback_3, NULL);
    buttons[3].color_idle    = COLOR_MAGENTA;
    buttons[3].color_pressed = COLOR_ORANGE;

    // Draw all buttons initially
    for (int i = 0; i < 4; i++) {
        touch_button_draw(gui, &buttons[i]);
    }

    // Draw click count display area
    gui_draw_string(gui, 10, 300, "Click Counts:", COLOR_WHITE, COLOR_BLACK, 2);

    // Touch handling loop
    esp_lcd_touch_point_data_t touch_data;
    uint8_t touch_cnt = 0;
    int frame_count   = 0;

    while (frame_count < 600) { // Run for ~60 seconds
        // Read touch data
        esp_lcd_touch_read_data(tp);
        esp_err_t ret = esp_lcd_touch_get_data(tp, &touch_data, &touch_cnt, 1);

        bool touch_active = (ret == ESP_OK && touch_cnt > 0);
        uint16_t touch_x  = touch_active ? touch_data.x : 0;
        uint16_t touch_y  = touch_active ? touch_data.y : 0;

        // Update all buttons
        for (int i = 0; i < 4; i++) {
            touch_button_update(gui, &buttons[i], touch_x, touch_y, touch_active);
        }

        // Update click count display (optimized: check if count changed before redraw)
        static int last_counts[4] = {-1, -1, -1, -1}; // Initialize to invalid value
        char count_str[32];
        for (int i = 0; i < 4; i++) {
            // Only redraw if count changed (local redraw optimization)
            if (button_click_count[i] != last_counts[i]) {
                // Clear previous text area first
                gui_draw_filled_rect(gui, 10 + i * 120, 330, 10 + i * 120 + 80, 346, COLOR_BLACK);

                // Draw new count
                sprintf(count_str, "B%d: %d", i + 1, button_click_count[i]);
                gui_draw_string(gui, 10 + i * 120, 330, count_str, buttons[i].color_idle, COLOR_BLACK, 1);

                last_counts[i] = button_click_count[i];
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20)); // 50 FPS (20ms interval) - Optimized sampling rate
        frame_count++;
    }

    // Display demo completion
    gui_clear_screen(gui, COLOR_BLACK);
    gui_draw_string(gui, 10, 10, "DEMO COMPLETE", COLOR_WHITE, COLOR_BLACK, 4);

    char summary_str[64];
    int total_clicks = 0;
    for (int i = 0; i < 4; i++) {
        total_clicks += button_click_count[i];
    }
    sprintf(summary_str, "Total clicks: %d", total_clicks);
    gui_draw_string(gui, 10, 60, summary_str, COLOR_GREEN, COLOR_BLACK, 3);

    ESP_LOGI(TAG, "Demo completed. Total clicks: %d", total_clicks);

    vTaskDelay(pdMS_TO_TICKS(3000));
}