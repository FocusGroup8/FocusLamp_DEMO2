/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "simple_gui.h"
#include "esp_lcd_touch.h"

/**
 * @brief Touch GUI library for interactive applications
 * 
 * This library provides touch-enabled GUI components like buttons,
 * sliders, and other interactive elements.
 */

// Button states
typedef enum {
    BUTTON_STATE_IDLE,      // Button is not pressed
    BUTTON_STATE_PRESSED,   // Button is being pressed
    BUTTON_STATE_RELEASED,  // Button was just released
} button_state_t;

// Button callback function type
typedef void (*button_callback_t)(void *user_data);

/**
 * @brief Touch button component structure
 */
typedef struct {
    int x;                  // X position (top-left corner)
    int y;                  // Y position (top-left corner)
    int width;              // Button width
    int height;             // Button height
    uint32_t color_idle;    // Color when idle
    uint32_t color_pressed; // Color when pressed
    uint32_t text_color;    // Text color
    const char *text;       // Button text label
    uint8_t text_size;      // Text size (1-4)
    button_state_t state;   // Current button state
    button_callback_t callback; // Callback function when pressed
    void *user_data;        // User data for callback
    
    // Performance optimization fields
    uint32_t last_press_time; // Timestamp of last press (for debounce)
    uint16_t debounce_ms;     // Debounce interval in milliseconds
} touch_button_t;

/**
 * @brief Initialize a touch button
 * 
 * @param button Pointer to button structure
 * @param x X position
 * @param y Y position
 * @param width Button width
 * @param height Button height
 * @param text Button label text
 * @param callback Callback function (optional)
 * @param user_data User data for callback (optional)
 */
void touch_button_init(touch_button_t *button, int x, int y, int width, int height,
                       const char *text, button_callback_t callback, void *user_data);

/**
 * @brief Draw a touch button on the screen
 * 
 * @param gui Pointer to GUI context
 * @param button Pointer to button structure
 */
void touch_button_draw(simple_gui_t *gui, touch_button_t *button);

/**
 * @brief Check if a touch point is inside a button
 * 
 * @param button Pointer to button structure
 * @param touch_x Touch point X coordinate
 * @param touch_y Touch point Y coordinate
 * @return true if touch is inside button, false otherwise
 */
bool touch_button_check_hit(touch_button_t *button, uint16_t touch_x, uint16_t touch_y);

/**
 * @brief Update button state based on touch input
 * 
 * @param gui Pointer to GUI context
 * @param button Pointer to button structure
 * @param touch_x Touch point X coordinate (0 if no touch)
 * @param touch_y Touch point Y coordinate (0 if no touch)
 * @param touch_active true if touch is active, false otherwise
 * @return true if button state changed, false otherwise
 */
bool touch_button_update(simple_gui_t *gui, touch_button_t *button,
                         uint16_t touch_x, uint16_t touch_y, bool touch_active);

/**
 * @brief Create a demo application with multiple touch buttons
 * 
 * @param gui Pointer to GUI context
 * @param tp Pointer to touch handle
 */
void touch_gui_demo(simple_gui_t *gui, esp_lcd_touch_handle_t tp);