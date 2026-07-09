/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TOUCH_GAME_H
#define TOUCH_GAME_H

#include "esp_lcd_touch.h"
#include "gesture_recognition.h"
#include "simple_gui.h"

/**
 * @brief Touch game demonstration using gesture recognition
 *
 * Game features:
 * - Swipe: Control ball movement
 * - Pinch: Adjust ball size
 * - Rotation: Rotate ball animation
 * - Long press: Pause/resume game
 */

void touch_game_demo(simple_gui_t *gui, esp_lcd_touch_handle_t tp);

#endif // TOUCH_GAME_H