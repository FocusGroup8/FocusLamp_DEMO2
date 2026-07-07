/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_lcd_touch.h"
#include "simple_gui.h"

/**
 * @brief Run the demo selected via Kconfig (EXAMPLE_DEMO_*).
 *
 * @param gui  GUI context (double-buffer mode)
 * @param tp   Touch handle
 */
void app_run_demo(simple_gui_t *gui, esp_lcd_touch_handle_t tp);
