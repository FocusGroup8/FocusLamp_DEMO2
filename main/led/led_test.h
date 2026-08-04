/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file led_test.h
 * @brief LED controller test routines
 */

/**
 * @brief Run LED controller test sequence
 *
 * Executes the following test sequence:
 * 1. Initialize LED controller
 * 2. Test individual channel brightness (A then B, ramp up)
 * 3. Test both channels simultaneously
 * 4. Test color temperature adjustment (warm -> cool)
 * 5. Test brightness with CCT
 * 6. Turn off
 *
 * @return ESP_OK if all tests pass
 */
esp_err_t led_test_run(void);

#ifdef __cplusplus
}
#endif
