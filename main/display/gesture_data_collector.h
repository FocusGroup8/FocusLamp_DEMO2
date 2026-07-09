/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_lcd_touch.h"
#include "simple_gui.h"

#include <stdint.h>

/**
 * @brief Gesture data collector for debugging and calibration
 *
 * This module collects raw touch data and helps calibrate gesture recognition thresholds
 */

void gesture_data_collector_demo(simple_gui_t *gui, esp_lcd_touch_handle_t tp);