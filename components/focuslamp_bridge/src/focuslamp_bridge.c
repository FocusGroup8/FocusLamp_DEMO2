/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "focuslamp_bridge.h"
#include "focuslamp_bridge_config.h"
#include "mipi_dsi_bridge.h"

#include "esp_mcp_data.h"
#include "esp_mcp_engine.h"
#include "esp_mcp_property.h"
#include "esp_mcp_tool.h"

static const char *TAG = "FOCUSLAMP_BRIDGE";

/*---------------------------------------------------------------
 * CRC16-CCITT for data integrity verification
 *-------------------------------------------------------------*/
static uint16_t crc16_ccitt(const uint8_t *data, int len) {
  uint16_t crc = 0xFFFF;
  for (int i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (int j = 0; j < 8; j++) {
      if (crc & 0x8000) {
        crc = (crc << 1) ^ 0x1021;
      } else {
        crc <<= 1;
      }
    }
  }
  return crc;
}

/*---------------------------------------------------------------
 * State
 *-------------------------------------------------------------*/
static bool s_initialized = false;
static char s_target_ip[48] = {0}; /* IP:port buffer */

/*---------------------------------------------------------------
 * HTTP client helper: send POST to FocusLamp REST API
 *
 * Sends a POST request with optional JSON body to the target
 * endpoint, reads the response into resp_buf.
 *
 * @param path       REST API path (e.g., "/api/led/on")
 * @param post_body  JSON body string (NULL for no body)
 * @param resp_buf   Response buffer (output, can be NULL)
 * @param resp_size  Response buffer size
 * @return ESP_OK on HTTP 200, error code otherwise
 *-------------------------------------------------------------*/
static esp_err_t http_post(const char *path, const char *post_body,
                           char *resp_buf, int resp_size) {
  if (!s_initialized || s_target_ip[0] == '\0') {
    ESP_LOGE(TAG, "Bridge not initialized or target IP not set");
    return ESP_ERR_INVALID_STATE;
  }

  char url[128];
  snprintf(url, sizeof(url), "http://%s%s", s_target_ip, path);

  esp_http_client_config_t config = {
      .url = url,
      .method = HTTP_METHOD_POST,
      .timeout_ms = FOCUSLAMP_BRIDGE_HTTP_TIMEOUT_MS,
  };

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    ESP_LOGE(TAG, "Failed to create HTTP client for %s", url);
    return ESP_ERR_NO_MEM;
  }

  esp_http_client_set_header(client, "Content-Type", "application/json");

  if (post_body) {
    esp_http_client_set_post_field(client, post_body, strlen(post_body));
    /* CRC16 checksum of request body for integrity verification */
    uint16_t crc = crc16_ccitt((const uint8_t *)post_body, strlen(post_body));
    char crc_hdr[8];
    snprintf(crc_hdr, sizeof(crc_hdr), "%04X", crc);
    esp_http_client_set_header(client, "X-CRC16", crc_hdr);
  }

  esp_err_t err = esp_http_client_perform(client);
  int status_code = 0;

  if (err == ESP_OK) {
    status_code = esp_http_client_get_status_code(client);
    int content_length = esp_http_client_get_content_length(client);

    if (resp_buf && resp_size > 0 && content_length > 0) {
      int read_len = esp_http_client_read(client, resp_buf, resp_size - 1);
      if (read_len >= 0) {
        resp_buf[read_len] = '\0';
        /* Verify response CRC16 if X-CRC16 header is present */
        char *resp_crc_val = NULL;
        if (esp_http_client_get_header(client, "X-CRC16", &resp_crc_val) ==
                ESP_OK &&
            resp_crc_val != NULL) {
          uint16_t recv_crc = (uint16_t)strtol(resp_crc_val, NULL, 16);
          uint16_t calc_crc = crc16_ccitt((const uint8_t *)resp_buf, read_len);
          if (recv_crc != calc_crc) {
            ESP_LOGW(TAG, "Response CRC mismatch: recv=%04X calc=%04X",
                     recv_crc, calc_crc);
          }
        }
      }
    }

    if (status_code != 200) {
      ESP_LOGW(TAG, "HTTP POST %s -> %d", path, status_code);
      err = ESP_ERR_INVALID_RESPONSE;
    }
  } else {
    ESP_LOGW(TAG, "HTTP POST %s failed: %s", path, esp_err_to_name(err));
  }

  esp_http_client_cleanup(client);
  return err;
}

/*---------------------------------------------------------------
 * HTTP GET helper for ping/status check
 *-------------------------------------------------------------*/
static esp_err_t http_get(const char *path, char *resp_buf, int resp_size) {
  if (!s_initialized || s_target_ip[0] == '\0') {
    return ESP_ERR_INVALID_STATE;
  }

  char url[128];
  snprintf(url, sizeof(url), "http://%s%s", s_target_ip, path);

  esp_http_client_config_t config = {
      .url = url,
      .method = HTTP_METHOD_GET,
      .timeout_ms = FOCUSLAMP_BRIDGE_HTTP_TIMEOUT_MS,
  };

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    return ESP_ERR_NO_MEM;
  }

  esp_err_t err = esp_http_client_perform(client);
  int status_code = 0;

  if (err == ESP_OK) {
    status_code = esp_http_client_get_status_code(client);
    int content_length = esp_http_client_get_content_length(client);

    if (resp_buf && resp_size > 0 && content_length > 0) {
      int read_len = esp_http_client_read(client, resp_buf, resp_size - 1);
      if (read_len >= 0) {
        resp_buf[read_len] = '\0';
      }
    }

    if (status_code != 200) {
      ESP_LOGW(TAG, "HTTP GET %s -> %d", path, status_code);
      err = ESP_ERR_INVALID_RESPONSE;
    }
  }

  esp_http_client_cleanup(client);
  return err;
}

/*---------------------------------------------------------------
 * Client API: device control commands
 *-------------------------------------------------------------*/
#if (FOCUSLAMP_BRIDGE_ENABLE == 1)

/* LED control */
esp_err_t focuslamp_bridge_led_on(int brightness) {
  char body[32];
  if (brightness > 0) {
    snprintf(body, sizeof(body), "{\"brightness\":%d}", brightness);
  } else {
    snprintf(body, sizeof(body), "{\"brightness\":100}");
  }
  char resp[64] = {0};
  return http_post("/api/led/on", body, resp, sizeof(resp));
}

esp_err_t focuslamp_bridge_led_off(void) {
  char resp[64] = {0};
  return http_post("/api/led/off", NULL, resp, sizeof(resp));
}

esp_err_t focuslamp_bridge_led_set_brightness(int brightness) {
  char body[32];
  snprintf(body, sizeof(body), "{\"brightness\":%d}", brightness);
  char resp[64] = {0};
  return http_post("/api/led/brightness", body, resp, sizeof(resp));
}

esp_err_t focuslamp_bridge_led_set_effect(const char *effect) {
  if (!effect) return ESP_ERR_INVALID_ARG;
  char body[64];
  snprintf(body, sizeof(body), "{\"effect\":\"%s\"}", effect);
  char resp[64] = {0};
  return http_post("/api/led/effect", body, resp, sizeof(resp));
}

esp_err_t focuslamp_bridge_led_set_color(int r, int g, int b) {
  char body[64];
  snprintf(body, sizeof(body), "{\"r\":%d,\"g\":%d,\"b\":%d}", r, g, b);
  char resp[64] = {0};
  return http_post("/api/led/color", body, resp, sizeof(resp));
}

/* Display control */
esp_err_t focuslamp_bridge_display_on(void) {
  char resp[64] = {0};
  return http_post("/api/display/on", NULL, resp, sizeof(resp));
}

esp_err_t focuslamp_bridge_display_off(void) {
  char resp[64] = {0};
  return http_post("/api/display/off", NULL, resp, sizeof(resp));
}

esp_err_t focuslamp_bridge_display_set_brightness(int level) {
  char body[32];
  snprintf(body, sizeof(body), "{\"level\":%d}", level);
  char resp[64] = {0};
  return http_post("/api/display/brightness", body, resp, sizeof(resp));
}

/* LCD control */
esp_err_t focuslamp_bridge_lcd_next_page(void) {
  char resp[64] = {0};
  return http_post("/api/lcd/page/next", NULL, resp, sizeof(resp));
}

esp_err_t focuslamp_bridge_lcd_set_expression(const char *expression) {
  if (!expression) return ESP_ERR_INVALID_ARG;
  char body[64];
  snprintf(body, sizeof(body), "{\"expression\":\"%s\"}", expression);
  char resp[64] = {0};
  return http_post("/api/lcd/expression", body, resp, sizeof(resp));
}

esp_err_t focuslamp_bridge_lcd_enable_blink(void) {
  char resp[64] = {0};
  return http_post("/api/lcd/blink/enable", NULL, resp, sizeof(resp));
}

esp_err_t focuslamp_bridge_lcd_disable_blink(void) {
  char resp[64] = {0};
  return http_post("/api/lcd/blink/disable", NULL, resp, sizeof(resp));
}

/* Servo control */
esp_err_t focuslamp_bridge_servo_set_position(int servo_id, int position) {
  char body[64];
  snprintf(body, sizeof(body), "{\"servo_id\":%d,\"position\":%d}", servo_id, position);
  char resp[64] = {0};
  return http_post("/api/servo/position", body, resp, sizeof(resp));
}

esp_err_t focuslamp_bridge_servo_go_home(int time_ms) {
  char body[32];
  if (time_ms > 0) {
    snprintf(body, sizeof(body), "{\"time_ms\":%d}", time_ms);
  } else {
    body[0] = '\0';
  }
  char resp[64] = {0};
  return http_post("/api/servo/home", body[0] ? body : NULL, resp, sizeof(resp));
}

/* Focus mode control */
esp_err_t focuslamp_bridge_focus_start(int duration_minutes) {
  char body[32];
  snprintf(body, sizeof(body), "{\"duration\":%d}", duration_minutes);
  char resp[64] = {0};
  return http_post("/api/focus/start", body, resp, sizeof(resp));
}

esp_err_t focuslamp_bridge_focus_stop(void) {
  char resp[64] = {0};
  return http_post("/api/focus/stop", NULL, resp, sizeof(resp));
}

/* Companion mode control */
esp_err_t focuslamp_bridge_companion_start(int duration) {
  if (duration < 0) duration = 0;
  char body[32];
  snprintf(body, sizeof(body), "{\"duration\":%d}", duration);
  char resp[64] = {0};
  return http_post("/api/companion/start", body, resp, sizeof(resp));
}

esp_err_t focuslamp_bridge_companion_stop(void) {
  char resp[64] = {0};
  return http_post("/api/companion/stop", NULL, resp, sizeof(resp));
}

/* Motion control */
esp_err_t focuslamp_bridge_motion_wave(void) {
  char resp[64] = {0};
  return http_post("/api/motion/wave", NULL, resp, sizeof(resp));
}

esp_err_t focuslamp_bridge_motion_nod(void) {
  char resp[64] = {0};
  return http_post("/api/motion/nod", NULL, resp, sizeof(resp));
}

esp_err_t focuslamp_bridge_motion_shake(void) {
  char resp[64] = {0};
  return http_post("/api/motion/shake", NULL, resp, sizeof(resp));
}

esp_err_t focuslamp_bridge_motion_dance(void) {
  char resp[64] = {0};
  return http_post("/api/motion/dance", NULL, resp, sizeof(resp));
}

esp_err_t focuslamp_bridge_motion_greet(void) {
  char resp[64] = {0};
  return http_post("/api/motion/greet", NULL, resp, sizeof(resp));
}

esp_err_t focuslamp_bridge_motion_home(void) {
  char resp[64] = {0};
  return http_post("/api/motion/home", NULL, resp, sizeof(resp));
}

esp_err_t focuslamp_bridge_chat_state(bool active) {
  char body[32];
  snprintf(body, sizeof(body), "{\"active\":%s}", active ? "true" : "false");
  char resp[64] = {0};
  esp_err_t ret = http_post("/api/chat/state", body, resp, sizeof(resp));
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Chat state report (active=%d) failed: %s", active,
             esp_err_to_name(ret));
  }
  return ret;
}

/*---------------------------------------------------------------
 * MCP tool callbacks
 *-------------------------------------------------------------*/
static esp_mcp_value_t
mcp_tool_led_on(const esp_mcp_property_list_t *properties) {
  int brightness = esp_mcp_property_list_get_property_int(properties, "brightness");
  if (brightness <= 0) brightness = 100;
  ESP_LOGI(TAG, "[MCP] focuslamp.led.on: brightness=%d", brightness);
  return esp_mcp_value_create_bool(focuslamp_bridge_led_on(brightness) == ESP_OK);
}

static esp_mcp_value_t
mcp_tool_led_off(const esp_mcp_property_list_t *properties) {
  (void)properties;
  ESP_LOGI(TAG, "[MCP] focuslamp.led.off");
  return esp_mcp_value_create_bool(focuslamp_bridge_led_off() == ESP_OK);
}

static esp_mcp_value_t
mcp_tool_led_set_brightness(const esp_mcp_property_list_t *properties) {
  int brightness = esp_mcp_property_list_get_property_int(properties, "brightness");
  if (brightness <= 0) brightness = 100;
  ESP_LOGI(TAG, "[MCP] focuslamp.led.set_brightness: %d", brightness);
  return esp_mcp_value_create_bool(focuslamp_bridge_led_set_brightness(brightness) == ESP_OK);
}

static esp_mcp_value_t
mcp_tool_led_set_effect(const esp_mcp_property_list_t *properties) {
  const char *effect = esp_mcp_property_list_get_property_string(properties, "effect");
  if (!effect || effect[0] == '\0') {
    ESP_LOGW(TAG, "[MCP] focuslamp.led.set_effect: missing effect");
    return esp_mcp_value_create_bool(false);
  }
  ESP_LOGI(TAG, "[MCP] focuslamp.led.set_effect: %s", effect);
  return esp_mcp_value_create_bool(focuslamp_bridge_led_set_effect(effect) == ESP_OK);
}

static esp_mcp_value_t
mcp_tool_led_set_color(const esp_mcp_property_list_t *properties) {
  int r = esp_mcp_property_list_get_property_int(properties, "r");
  int g = esp_mcp_property_list_get_property_int(properties, "g");
  int b = esp_mcp_property_list_get_property_int(properties, "b");
  ESP_LOGI(TAG, "[MCP] focuslamp.led.set_color: (%d,%d,%d)", r, g, b);
  return esp_mcp_value_create_bool(focuslamp_bridge_led_set_color(r, g, b) == ESP_OK);
}

static esp_mcp_value_t
mcp_tool_display_on(const esp_mcp_property_list_t *properties) {
  (void)properties;
  ESP_LOGI(TAG, "[MCP] focuslamp.display.on");
  return esp_mcp_value_create_bool(focuslamp_bridge_display_on() == ESP_OK);
}

static esp_mcp_value_t
mcp_tool_display_off(const esp_mcp_property_list_t *properties) {
  (void)properties;
  ESP_LOGI(TAG, "[MCP] focuslamp.display.off");
  return esp_mcp_value_create_bool(focuslamp_bridge_display_off() == ESP_OK);
}

static esp_mcp_value_t
mcp_tool_display_set_brightness(const esp_mcp_property_list_t *properties) {
  int level = esp_mcp_property_list_get_property_int(properties, "level");
  ESP_LOGI(TAG, "[MCP] focuslamp.display.set_brightness: %d", level);
  return esp_mcp_value_create_bool(focuslamp_bridge_display_set_brightness(level) == ESP_OK);
}

static esp_mcp_value_t
mcp_tool_lcd_next_page(const esp_mcp_property_list_t *properties) {
  (void)properties;
  ESP_LOGI(TAG, "[MCP] focuslamp.lcd.next_page");
  return esp_mcp_value_create_bool(focuslamp_bridge_lcd_next_page() == ESP_OK);
}

static esp_mcp_value_t
mcp_tool_lcd_set_expression(const esp_mcp_property_list_t *properties) {
  const char *expression = esp_mcp_property_list_get_property_string(properties, "expression");
  if (!expression || expression[0] == '\0') {
    ESP_LOGW(TAG, "[MCP] focuslamp.lcd.set_expression: missing expression");
    return esp_mcp_value_create_bool(false);
  }
  ESP_LOGI(TAG, "[MCP] focuslamp.lcd.set_expression: %s", expression);
  return esp_mcp_value_create_bool(focuslamp_bridge_lcd_set_expression(expression) == ESP_OK);
}

static esp_mcp_value_t
mcp_tool_lcd_enable_blink(const esp_mcp_property_list_t *properties) {
  (void)properties;
  ESP_LOGI(TAG, "[MCP] focuslamp.lcd.enable_blink");
  return esp_mcp_value_create_bool(focuslamp_bridge_lcd_enable_blink() == ESP_OK);
}

static esp_mcp_value_t
mcp_tool_lcd_disable_blink(const esp_mcp_property_list_t *properties) {
  (void)properties;
  ESP_LOGI(TAG, "[MCP] focuslamp.lcd.disable_blink");
  return esp_mcp_value_create_bool(focuslamp_bridge_lcd_disable_blink() == ESP_OK);
}

static esp_mcp_value_t
mcp_tool_servo_set_position(const esp_mcp_property_list_t *properties) {
  int servo_id = esp_mcp_property_list_get_property_int(properties, "servo_id");
  int position = esp_mcp_property_list_get_property_int(properties, "position");
  ESP_LOGI(TAG, "[MCP] focuslamp.servo.set_position: id=%d pos=%d", servo_id, position);
  return esp_mcp_value_create_bool(focuslamp_bridge_servo_set_position(servo_id, position) == ESP_OK);
}

static esp_mcp_value_t
mcp_tool_servo_go_home(const esp_mcp_property_list_t *properties) {
  int time_ms = esp_mcp_property_list_get_property_int(properties, "time_ms");
  ESP_LOGI(TAG, "[MCP] focuslamp.servo.go_home: time_ms=%d", time_ms);
  return esp_mcp_value_create_bool(focuslamp_bridge_servo_go_home(time_ms) == ESP_OK);
}

static esp_mcp_value_t
mcp_tool_focus_start(const esp_mcp_property_list_t *properties) {
  int duration = esp_mcp_property_list_get_property_int(properties, "duration");
  if (duration <= 0) duration = 30;
  ESP_LOGI(TAG, "[MCP] focuslamp.focus.start: duration=%d", duration);

  /* Base board: focus mode start. Head board: sync mode for VLM/presence. */
  bool ok = focuslamp_bridge_focus_start(duration) == ESP_OK;
  if (mipi_dsi_bridge_mode_set("focus") != ESP_OK) {
    ESP_LOGW(TAG, "[MCP] focus.start: head board mode sync failed");
  }
  return esp_mcp_value_create_bool(ok);
}

static esp_mcp_value_t
mcp_tool_focus_stop(const esp_mcp_property_list_t *properties) {
  (void)properties;
  ESP_LOGI(TAG, "[MCP] focuslamp.focus.stop");

  bool ok = focuslamp_bridge_focus_stop() == ESP_OK;
  if (mipi_dsi_bridge_mode_set("normal") != ESP_OK) {
    ESP_LOGW(TAG, "[MCP] focus.stop: head board mode sync failed");
  }
  return esp_mcp_value_create_bool(ok);
}

static esp_mcp_value_t
mcp_tool_companion_start(const esp_mcp_property_list_t *properties) {
  (void)properties;
  ESP_LOGI(TAG, "[MCP] focuslamp.companion.start");

  bool ok = focuslamp_bridge_companion_start(0) == ESP_OK;
  if (mipi_dsi_bridge_mode_set("companion") != ESP_OK) {
    ESP_LOGW(TAG, "[MCP] companion.start: head board mode sync failed");
  }
  return esp_mcp_value_create_bool(ok);
}

static esp_mcp_value_t
mcp_tool_companion_stop(const esp_mcp_property_list_t *properties) {
  (void)properties;
  ESP_LOGI(TAG, "[MCP] focuslamp.companion.stop");

  bool ok = focuslamp_bridge_companion_stop() == ESP_OK;
  if (mipi_dsi_bridge_mode_set("normal") != ESP_OK) {
    ESP_LOGW(TAG, "[MCP] companion.stop: head board mode sync failed");
  }
  return esp_mcp_value_create_bool(ok);
}

static esp_mcp_value_t
mcp_tool_motion_wave(const esp_mcp_property_list_t *properties) {
  (void)properties;
  ESP_LOGI(TAG, "[MCP] focuslamp.arm.wave");
  return esp_mcp_value_create_bool(focuslamp_bridge_motion_wave() == ESP_OK);
}

static esp_mcp_value_t
mcp_tool_motion_nod(const esp_mcp_property_list_t *properties) {
  (void)properties;
  ESP_LOGI(TAG, "[MCP] focuslamp.arm.nod");
  return esp_mcp_value_create_bool(focuslamp_bridge_motion_nod() == ESP_OK);
}

static esp_mcp_value_t
mcp_tool_motion_shake(const esp_mcp_property_list_t *properties) {
  (void)properties;
  ESP_LOGI(TAG, "[MCP] focuslamp.arm.shake");
  return esp_mcp_value_create_bool(focuslamp_bridge_motion_shake() == ESP_OK);
}

static esp_mcp_value_t
mcp_tool_motion_dance(const esp_mcp_property_list_t *properties) {
  (void)properties;
  ESP_LOGI(TAG, "[MCP] focuslamp.arm.dance");
  return esp_mcp_value_create_bool(focuslamp_bridge_motion_dance() == ESP_OK);
}

static esp_mcp_value_t
mcp_tool_motion_greet(const esp_mcp_property_list_t *properties) {
  (void)properties;
  ESP_LOGI(TAG, "[MCP] focuslamp.arm.greet");
  return esp_mcp_value_create_bool(focuslamp_bridge_motion_greet() == ESP_OK);
}

static esp_mcp_value_t
mcp_tool_motion_home(const esp_mcp_property_list_t *properties) {
  (void)properties;
  ESP_LOGI(TAG, "[MCP] focuslamp.arm.home");
  return esp_mcp_value_create_bool(focuslamp_bridge_motion_home() == ESP_OK);
}

/*---------------------------------------------------------------
 * MCP tool registration
 *-------------------------------------------------------------*/
esp_err_t focuslamp_bridge_register_mcp_tools(esp_mcp_t *mcp) {
  ESP_RETURN_ON_FALSE(mcp, ESP_ERR_INVALID_ARG, TAG, "Invalid MCP engine");

  /* LED tools (5) */
  esp_mcp_tool_t *t;
  t = esp_mcp_tool_create("self.focuslamp.led.on", "开启底部台灯LED灯(可指定亮度0-100)", mcp_tool_led_on);
  if (t) { esp_mcp_property_t *p = esp_mcp_property_create_with_range("brightness", 0, 100); esp_mcp_tool_add_property(t, p); esp_mcp_add_tool(mcp, t); }

  t = esp_mcp_tool_create("self.focuslamp.led.off", "关闭底部台灯LED灯", mcp_tool_led_off);
  if (t) esp_mcp_add_tool(mcp, t);

  t = esp_mcp_tool_create("self.focuslamp.led.set_brightness", "设置底部台灯LED亮度(0-100)", mcp_tool_led_set_brightness);
  if (t) { esp_mcp_property_t *p = esp_mcp_property_create_with_range("brightness", 0, 100); esp_mcp_tool_add_property(t, p); esp_mcp_add_tool(mcp, t); }

  t = esp_mcp_tool_create("self.focuslamp.led.set_effect", "设置底部台灯LED效果(steady/breathing/rainbow/blinking)", mcp_tool_led_set_effect);
  if (t) { esp_mcp_property_t *p = esp_mcp_property_create("effect", ESP_MCP_PROPERTY_TYPE_STRING); esp_mcp_tool_add_property(t, p); esp_mcp_add_tool(mcp, t); }

  t = esp_mcp_tool_create("self.focuslamp.led.set_color", "设置底部台灯LED颜色(r/g/b各0-255)", mcp_tool_led_set_color);
  if (t) {
    esp_mcp_property_t *p1 = esp_mcp_property_create_with_range("r", 0, 255);
    esp_mcp_tool_add_property(t, p1);
    esp_mcp_property_t *p2 = esp_mcp_property_create_with_range("g", 0, 255);
    esp_mcp_tool_add_property(t, p2);
    esp_mcp_property_t *p3 = esp_mcp_property_create_with_range("b", 0, 255);
    esp_mcp_tool_add_property(t, p3);
    esp_mcp_add_tool(mcp, t);
  }

  /* Display tools (3) */
  t = esp_mcp_tool_create("self.focuslamp.display.on", "开启底部台灯显示屏", mcp_tool_display_on);
  if (t) esp_mcp_add_tool(mcp, t);

  t = esp_mcp_tool_create("self.focuslamp.display.off", "关闭底部台灯显示屏", mcp_tool_display_off);
  if (t) esp_mcp_add_tool(mcp, t);

  t = esp_mcp_tool_create("self.focuslamp.display.set_brightness", "设置底部台灯显示屏亮度(0-255)", mcp_tool_display_set_brightness);
  if (t) { esp_mcp_property_t *p = esp_mcp_property_create_with_range("level", 0, 255); esp_mcp_tool_add_property(t, p); esp_mcp_add_tool(mcp, t); }

  /* LCD tools (4) */
  t = esp_mcp_tool_create("self.focuslamp.lcd.next_page", "切换底部台灯LCD下一页", mcp_tool_lcd_next_page);
  if (t) esp_mcp_add_tool(mcp, t);

  t = esp_mcp_tool_create("self.focuslamp.lcd.set_expression", "设置底部台灯LCD表情(normal/happy/sad/angry/surprised/sleepy)", mcp_tool_lcd_set_expression);
  if (t) { esp_mcp_property_t *p = esp_mcp_property_create("expression", ESP_MCP_PROPERTY_TYPE_STRING); esp_mcp_tool_add_property(t, p); esp_mcp_add_tool(mcp, t); }

  t = esp_mcp_tool_create("self.focuslamp.lcd.enable_blink", "启用底部台灯LCD自动眨眼", mcp_tool_lcd_enable_blink);
  if (t) esp_mcp_add_tool(mcp, t);

  t = esp_mcp_tool_create("self.focuslamp.lcd.disable_blink", "禁用底部台灯LCD自动眨眼", mcp_tool_lcd_disable_blink);
  if (t) esp_mcp_add_tool(mcp, t);

  /* Servo tools (2) */
  t = esp_mcp_tool_create("self.focuslamp.servo.set_position", "设置底部台灯舵机位置(servo_id:0=EM3 0-3000, 1-4=LX 0-1000)", mcp_tool_servo_set_position);
  if (t) {
    esp_mcp_property_t *p1 = esp_mcp_property_create_with_range("servo_id", 0, 4);
    esp_mcp_tool_add_property(t, p1);
    esp_mcp_property_t *p2 = esp_mcp_property_create_with_range("position", 0, 3000);
    esp_mcp_tool_add_property(t, p2);
    esp_mcp_add_tool(mcp, t);
  }

  t = esp_mcp_tool_create("self.focuslamp.servo.go_home", "底部台灯所有舵机归零(可选time_ms 0-65535)", mcp_tool_servo_go_home);
  if (t) { esp_mcp_property_t *p = esp_mcp_property_create_with_range("time_ms", 0, 65535); esp_mcp_tool_add_property(t, p); esp_mcp_add_tool(mcp, t); }

  /* Focus mode tools (2) */
  t = esp_mcp_tool_create("self.focuslamp.focus.start", "开启底部台灯专注模式(可指定时长duration分钟,默认30)", mcp_tool_focus_start);
  if (t) { esp_mcp_property_t *p = esp_mcp_property_create_with_range("duration", 1, 180); esp_mcp_tool_add_property(t, p); esp_mcp_add_tool(mcp, t); }

  t = esp_mcp_tool_create("self.focuslamp.focus.stop", "停止底部台灯专注模式", mcp_tool_focus_stop);
  if (t) esp_mcp_add_tool(mcp, t);

  /* Companion mode tools (2) */
  t = esp_mcp_tool_create("self.focuslamp.companion.start", "开启底部台灯陪伴模式（聊天陪伴，机械臂活动+表情）", mcp_tool_companion_start);
  if (t) esp_mcp_add_tool(mcp, t);

  t = esp_mcp_tool_create("self.focuslamp.companion.stop", "关闭底部台灯陪伴模式", mcp_tool_companion_stop);
  if (t) esp_mcp_add_tool(mcp, t);

  /* Motion/arm tools (6) */
  t = esp_mcp_tool_create("self.focuslamp.arm.wave", "底部台灯机械臂挥手", mcp_tool_motion_wave);
  if (t) esp_mcp_add_tool(mcp, t);

  t = esp_mcp_tool_create("self.focuslamp.arm.nod", "底部台灯机械臂点头", mcp_tool_motion_nod);
  if (t) esp_mcp_add_tool(mcp, t);

  t = esp_mcp_tool_create("self.focuslamp.arm.shake", "底部台灯机械臂摇头", mcp_tool_motion_shake);
  if (t) esp_mcp_add_tool(mcp, t);

  t = esp_mcp_tool_create("self.focuslamp.arm.dance", "底部台灯机械臂跳舞", mcp_tool_motion_dance);
  if (t) esp_mcp_add_tool(mcp, t);

  t = esp_mcp_tool_create("self.focuslamp.arm.greet", "底部台灯机械臂打招呼", mcp_tool_motion_greet);
  if (t) esp_mcp_add_tool(mcp, t);

  t = esp_mcp_tool_create("self.focuslamp.arm.home", "底部台灯机械臂回零位", mcp_tool_motion_home);
  if (t) esp_mcp_add_tool(mcp, t);

  ESP_LOGI(TAG, "FocusLamp bridge MCP tools registered (24 tools)");
  return ESP_OK;
}

/*---------------------------------------------------------------
 * Init / Deinit
 *-------------------------------------------------------------*/

esp_err_t focuslamp_bridge_init(void) {
  ESP_RETURN_ON_FALSE(!s_initialized, ESP_ERR_INVALID_STATE, TAG,
                      "Already initialized");

  /* Read target IP from Kconfig */
  snprintf(s_target_ip, sizeof(s_target_ip), "%s",
           CONFIG_FOCUSLAMP_BRIDGE_TARGET_IP);

  if (s_target_ip[0] == '\0' || strcmp(s_target_ip, "0.0.0.0") == 0) {
    ESP_LOGW(TAG,
             "Target IP not configured (\"%s\"), bridge will not be functional",
             s_target_ip);
  } else {
    ESP_LOGI(TAG, "FocusLamp bridge initialized, target: %s", s_target_ip);
  }

  s_initialized = true;
  return ESP_OK;
}

void focuslamp_bridge_deinit(void) {
  if (!s_initialized) {
    return;
  }

  s_target_ip[0] = '\0';
  s_initialized = false;
  ESP_LOGI(TAG, "FocusLamp bridge deinitialized");
}

esp_err_t focuslamp_bridge_ping(void) {
  ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG,
                      "Not initialized");

  char resp[256] = {0};
  esp_err_t ret = http_get("/api/status", resp, sizeof(resp));

  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "Ping successful, status: %s", resp);
  } else {
    ESP_LOGW(TAG, "Ping failed: %s", esp_err_to_name(ret));
  }
  return ret;
}

#else /* FOCUSLAMP_BRIDGE_ENABLE == 0 */

/* Stub implementations when component is disabled */

esp_err_t focuslamp_bridge_init(void) { return ESP_ERR_NOT_SUPPORTED; }
void focuslamp_bridge_deinit(void) {}
esp_err_t focuslamp_bridge_register_mcp_tools(esp_mcp_t *mcp) {
  (void)mcp;
  return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t focuslamp_bridge_ping(void) { return ESP_ERR_NOT_SUPPORTED; }

#endif /* FOCUSLAMP_BRIDGE_ENABLE */
