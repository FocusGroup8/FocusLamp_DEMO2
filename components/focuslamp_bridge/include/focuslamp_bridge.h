/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "esp_mcp_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file focuslamp_bridge.h
 * @brief HTTP bridge for controlling FocusLamp_DEMO2 device from wifi_test
 *
 * Registers MCP tools that forward control commands to FocusLamp's
 * REST API endpoints via HTTP POST/GET requests.
 *
 * REST API endpoints (resource-style):
 *   GET  /api/status                 → query device status
 *   POST /api/led/on                 → turn on LED
 *   POST /api/led/off                → turn off LED
 *   POST /api/led/brightness         → set LED brightness
 *   POST /api/servo/move             → move servo to position
 *   POST /api/display/on             → turn on display
 *   POST /api/display/off            → turn off display
 *   POST /api/audio/play             → play audio
 *   POST /api/audio/stop             → stop audio
 *   POST /api/tools/{tool_name}      → invoke MCP tool
 *
 * All requests with JSON body carry X-CRC16 header (CCITT) for integrity.
 */

/**
 * @brief Initialize the FocusLamp bridge
 *
 * Reads FocusLamp device IP from configuration and prepares
 * the HTTP client for bridging requests.
 *
 * @return ESP_OK on success
 */
esp_err_t focuslamp_bridge_init(void);

/**
 * @brief Deinitialize the FocusLamp bridge
 */
void focuslamp_bridge_deinit(void);

/**
 * @brief Register MCP tools for FocusLamp control
 *
 * Registers MCP tools that forward commands to FocusLamp
 * via HTTP REST API. Must be called after focuslamp_bridge_init()
 * and with a valid MCP engine instance.
 *
 * @param mcp  MCP engine instance (from xiaozhi_manager_get_mcp_engine())
 * @return ESP_OK on success
 */
esp_err_t focuslamp_bridge_register_mcp_tools(esp_mcp_t *mcp);

/**
 * @brief Check if FocusLamp device is reachable
 *
 * Sends a GET /api/status request to verify connectivity.
 *
 * @return ESP_OK if device is reachable, error code otherwise
 */
esp_err_t focuslamp_bridge_ping(void);

/*---------------------------------------------------------------
 * Client API: device control commands (Phase B)
 *-------------------------------------------------------------*/

/* LED control */
esp_err_t focuslamp_bridge_led_on(int brightness);
esp_err_t focuslamp_bridge_led_off(void);
esp_err_t focuslamp_bridge_led_set_brightness(int brightness);
esp_err_t focuslamp_bridge_led_set_effect(const char *effect);
esp_err_t focuslamp_bridge_led_set_color(int r, int g, int b);

/* Display control */
esp_err_t focuslamp_bridge_display_on(void);
esp_err_t focuslamp_bridge_display_off(void);
esp_err_t focuslamp_bridge_display_set_brightness(int level);

/* LCD control */
esp_err_t focuslamp_bridge_lcd_next_page(void);
esp_err_t focuslamp_bridge_lcd_set_expression(const char *expression);
esp_err_t focuslamp_bridge_lcd_enable_blink(void);
esp_err_t focuslamp_bridge_lcd_disable_blink(void);

/* Servo control */
esp_err_t focuslamp_bridge_servo_set_position(int servo_id, int position);
esp_err_t focuslamp_bridge_servo_go_home(int time_ms);

/* Focus mode control */
esp_err_t focuslamp_bridge_focus_start(int duration_minutes);
esp_err_t focuslamp_bridge_focus_stop(void);

/* Motion/arm control */
esp_err_t focuslamp_bridge_motion_wave(void);
esp_err_t focuslamp_bridge_motion_nod(void);
esp_err_t focuslamp_bridge_motion_shake(void);
esp_err_t focuslamp_bridge_motion_dance(void);
esp_err_t focuslamp_bridge_motion_greet(void);
esp_err_t focuslamp_bridge_motion_home(void);

#ifdef __cplusplus
}
#endif
