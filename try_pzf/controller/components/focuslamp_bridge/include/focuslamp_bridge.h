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
 * @brief MCP adapter for local controller services.
 *
 * LED, display, servo/motion and mode commands call local services.
 * No HTTP client or remote base address is used. Reserved LCD page/expression/
 * blink APIs return ESP_ERR_NOT_SUPPORTED and are not advertised as MCP tools.
 * Head control remains a separate mipi_dsi_bridge responsibility.
 */

/**
 * @brief Initialize the FocusLamp bridge
 *
 * Enables the local adapter without reading a target IP or issuing a ping.
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
 * Registers MCP tools that invoke local services.
 * Must be called after focuslamp_bridge_init()
 * and with a valid MCP engine instance.
 *
 * @param mcp  MCP engine instance (from xiaozhi_manager_get_mcp_engine())
 * @return ESP_OK on success
 */
esp_err_t focuslamp_bridge_register_mcp_tools(esp_mcp_t *mcp);

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

/* Reserved local LCD page/expression/blink APIs. Always NOT_SUPPORTED in this
 * phase; no HTTP fallback and no corresponding MCP tools are registered. */
esp_err_t focuslamp_bridge_lcd_next_page(void);
esp_err_t focuslamp_bridge_lcd_set_expression(const char *expression);
esp_err_t focuslamp_bridge_lcd_enable_blink(void);
esp_err_t focuslamp_bridge_lcd_disable_blink(void);

/* Servo control */
esp_err_t focuslamp_bridge_servo_set_position(int servo_id, int position);
esp_err_t focuslamp_bridge_servo_go_home(int time_ms);

/* Local focus countdown state only: 1..180 minutes, paused during dialogue.
 * No LCD, light, white-noise or arm effects are applied in this phase.
 * Stop is idempotent and does not stop companion mode. */
esp_err_t focuslamp_bridge_focus_start(int duration_minutes);
esp_err_t focuslamp_bridge_focus_stop(void);

/* Local companion state; the app demo worker applies lighting/head actions
 * asynchronously. Duration is ignored. Stop only stops companion state. */
esp_err_t focuslamp_bridge_companion_start(int duration);
esp_err_t focuslamp_bridge_companion_stop(void);

/* Motion/arm control */
esp_err_t focuslamp_bridge_motion_wave(void);
esp_err_t focuslamp_bridge_motion_nod(void);
esp_err_t focuslamp_bridge_motion_shake(void);
esp_err_t focuslamp_bridge_motion_dance(void);
esp_err_t focuslamp_bridge_motion_greet(void);
esp_err_t focuslamp_bridge_motion_home(void);

/* Stores dialogue activity locally so focus countdown pauses/resumes.
 * No HTTP request or fixed-IP fallback is used. */
esp_err_t focuslamp_bridge_chat_state(bool active);

#ifdef __cplusplus
}
#endif
