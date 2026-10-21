/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>

#include "cJSON.h"

#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "mipi_dsi_bridge.h"
#include "mipi_dsi_bridge_config.h"
#include "focuslamp_discovery.h"

#include "esp_mcp_data.h"
#include "esp_mcp_engine.h"
#include "esp_mcp_property.h"
#include "esp_mcp_tool.h"

static const char *TAG = "MIPI_BRIDGE";

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

/*---------------------------------------------------------------
 * Forward declarations for MCP tool callbacks
 *-------------------------------------------------------------*/
static esp_mcp_value_t
mcp_tool_camera_start(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t
mcp_tool_camera_stop(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t
mcp_tool_camera_set_quality(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t
mcp_tool_camera_set_fps(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t
mcp_tool_display_on(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t
mcp_tool_display_off(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t
mcp_tool_display_set_brightness(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t
mcp_tool_display_show_camera(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t
mcp_tool_led_on(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t
mcp_tool_led_off(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t
mcp_tool_led_set_brightness(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t
mcp_tool_led_set_level(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t
mcp_tool_led_set_color_temp(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t
mcp_tool_touch_wake(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t
mcp_tool_touch_brightness(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t
mcp_tool_touch_reset(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t
mcp_tool_eyes_set_expression(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t
mcp_tool_eyes_look_at(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t
mcp_tool_eyes_blink(const esp_mcp_property_list_t *properties);
static esp_mcp_value_t
mcp_tool_eyes_get_expression(const esp_mcp_property_list_t *properties);

/*---------------------------------------------------------------
 * HTTP client helper: send POST to mipi_dsi REST API
 *
 * Sends a POST request with optional JSON body to the target
 * endpoint, reads the response into resp_buf.
 *
 * @param path       REST API path (e.g., "/api/camera/start")
 * @param post_body  JSON body string (NULL for no body)
 * @param resp_buf   Response buffer (output, can be NULL)
 * @param resp_size  Response buffer size
 * @return ESP_OK on HTTP 200, error code otherwise
 *-------------------------------------------------------------*/
typedef struct {
  char *body;
  size_t capacity;
  size_t stored;
  size_t received;
  bool overflow;
  bool crc_present;
  bool crc_invalid;
  char crc_header[8];
} bridge_response_t;

/* perform() consumes the body. Capture response data during its callbacks,
 * never with read()/get_header() afterwards (get_header reads request headers). */
static esp_err_t bridge_http_event(esp_http_client_event_t *event) {
  bridge_response_t *response = event->user_data;
  if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key &&
      strcasecmp(event->header_key, "X-CRC16") == 0) {
    response->crc_present = true;
    size_t length = event->header_value ? strlen(event->header_value) : 0;
    if (length == 0 || length >= sizeof(response->crc_header)) {
      response->crc_invalid = true;
    } else {
      memcpy(response->crc_header, event->header_value, length + 1);
    }
  } else if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
    size_t length = (size_t)event->data_len;
    response->received += length;
    if (response->body && !response->overflow) {
      if (length > response->capacity - 1 - response->stored) {
        response->overflow = true;
      } else {
        memcpy(response->body + response->stored, event->data, length);
        response->stored += length;
        response->body[response->stored] = '\0';
      }
    }
  }
  return ESP_OK;
}

static void diagnose_response_crc(const bridge_response_t *response) {
  if (!response->crc_present || !response->body || response->overflow) {
    return;
  }
  char *end = NULL;
  unsigned long expected = strtoul(response->crc_header, &end, 16);
  if (response->crc_invalid || strlen(response->crc_header) != 4 ||
      !end || *end != '\0' || expected > UINT16_MAX ||
      strspn(response->crc_header, "0123456789abcdefABCDEF") != 4) {
    ESP_LOGW(TAG, "Invalid response X-CRC16 header (diagnostic only)");
    return;
  }
  uint16_t actual = crc16_ccitt((const uint8_t *)response->body,
                              (int)response->stored);
  if ((uint16_t)expected != actual) {
    ESP_LOGW(TAG, "Response CRC mismatch: recv=%04X calc=%04X (diagnostic only)",
             (unsigned)expected, actual);
  }
}

/* Only content-dependent queries reject collection overflow. Commands retain
 * their HTTP success semantics but never expose a truncated response. */
static esp_err_t http_request(esp_http_client_method_t method, const char *path,
                              const char *post_body, char *resp_buf,
                              int resp_size, bool require_body) {
  if ((resp_buf && resp_size <= 0) ||
      (require_body && (!resp_buf || resp_size <= 0))) {
    return ESP_ERR_INVALID_ARG;
  }
  if (resp_buf) {
    resp_buf[0] = '\0';
  }
  if (!s_initialized) {
    ESP_LOGE(TAG, "Bridge not initialized");
    return ESP_ERR_INVALID_STATE;
  }

  focuslamp_endpoint_t peer;
  if (focuslamp_discovery_get_peer(FOCUSLAMP_ROLE_HEAD, &peer) != ESP_OK) {
    return ESP_ERR_NOT_FOUND;
  }

  char url[128];
  snprintf(url, sizeof(url), "http://%s:%u%s", peer.host, (unsigned)peer.port, path);

  bridge_response_t response = {
      .body = resp_buf,
      .capacity = resp_buf ? (size_t)resp_size : 0,
  };

  esp_http_client_config_t config = {
      .url = url,
      .method = method,
      .timeout_ms = MIPI_DSI_BRIDGE_HTTP_TIMEOUT_MS,
      .event_handler = bridge_http_event,
      .user_data = &response,
      /* A bridge endpoint must answer directly, not redirect to another host
       * or mix multiple response bodies into this collection context. */
      .disable_auto_redirect = true,
  };

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    ESP_LOGE(TAG, "Failed to create HTTP client for %s", url);
    return ESP_ERR_NO_MEM;
  }

  esp_err_t err = ESP_OK;
  if (method == HTTP_METHOD_POST) {
    err = esp_http_client_set_header(client, "Content-Type", "application/json");
  }

  if (err == ESP_OK && post_body) {
    err = esp_http_client_set_post_field(client, post_body, strlen(post_body));
    /* CRC16 checksum of request body for integrity verification */
    uint16_t crc = crc16_ccitt((const uint8_t *)post_body, strlen(post_body));
    char crc_hdr[8];
    snprintf(crc_hdr, sizeof(crc_hdr), "%04X", crc);
    if (err == ESP_OK) {
      err = esp_http_client_set_header(client, "X-CRC16", crc_hdr);
    }
  }

  if (err == ESP_OK) {
    err = esp_http_client_perform(client);
  }
  int status_code = esp_http_client_get_status_code(client);
  int64_t content_length = esp_http_client_get_content_length(client);
  bool complete = esp_http_client_is_complete_data_received(client);
  bool chunked = esp_http_client_is_chunked_response(client);
  ESP_LOGI(TAG, "HTTP %s %s: status=%d content_length=%" PRId64
                " chunked=%d received=%u stored=%u overflow=%d complete=%d",
           method == HTTP_METHOD_POST ? "POST" : "GET", path, status_code,
           content_length, chunked, (unsigned)response.received,
           (unsigned)response.stored, response.overflow, complete);
  if (err == ESP_OK && (status_code != 200 || !complete)) {
    err = ESP_ERR_INVALID_RESPONSE;
  }
  if (response.overflow) {
    ESP_LOGW(TAG, "Response collection overflow: %s (%u bytes, capacity=%u)",
             path, (unsigned)response.received, (unsigned)response.capacity);
    if (require_body && err == ESP_OK) {
      err = ESP_ERR_INVALID_RESPONSE;
    }
  }
  if (err == ESP_OK && !response.overflow) {
    diagnose_response_crc(&response);
  }
  if (resp_buf && (err != ESP_OK || response.overflow)) {
    resp_buf[0] = '\0';
  }
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "HTTP %s failed: %s", path, esp_err_to_name(err));
  }

  esp_http_client_cleanup(client);
  return err;
}

static esp_err_t http_post(const char *path, const char *post_body,
                           char *resp_buf, int resp_size) {
  return http_request(HTTP_METHOD_POST, path, post_body, resp_buf, resp_size,
                      false);
}

/*---------------------------------------------------------------
 * HTTP GET helper for ping/status check
 *-------------------------------------------------------------*/
static esp_err_t http_get(const char *path, char *resp_buf, int resp_size,
                          bool require_body) {
  return http_request(HTTP_METHOD_GET, path, NULL, resp_buf, resp_size,
                      require_body);
}

/*---------------------------------------------------------------
 * MCP tool callbacks - Camera control
 *-------------------------------------------------------------*/
static esp_mcp_value_t
mcp_tool_camera_start(const esp_mcp_property_list_t *properties) {
  (void)properties;
  ESP_LOGI(TAG, "[MCP] mipi_dsi.camera.start");

  char resp[64] = {0};
  esp_err_t ret = http_post("/api/camera/start", NULL, resp, sizeof(resp));

  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "Camera start response: %s", resp);
    return esp_mcp_value_create_bool(true);
  }
  ESP_LOGW(TAG, "Camera start failed: %s", esp_err_to_name(ret));
  return esp_mcp_value_create_bool(false);
}

static esp_mcp_value_t
mcp_tool_camera_stop(const esp_mcp_property_list_t *properties) {
  (void)properties;
  ESP_LOGI(TAG, "[MCP] mipi_dsi.camera.stop");

  char resp[64] = {0};
  esp_err_t ret = http_post("/api/camera/stop", NULL, resp, sizeof(resp));

  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "Camera stop response: %s", resp);
    return esp_mcp_value_create_bool(true);
  }
  return esp_mcp_value_create_bool(false);
}

static esp_mcp_value_t
mcp_tool_camera_set_quality(const esp_mcp_property_list_t *properties) {
  int quality = esp_mcp_property_list_get_property_int(properties, "quality");
  ESP_LOGI(TAG, "[MCP] mipi_dsi.camera.set_quality: %d", quality);

  char body[32];
  snprintf(body, sizeof(body), "{\"quality\":%d}", quality);

  char resp[64] = {0};
  esp_err_t ret = http_post("/api/camera/quality", body, resp, sizeof(resp));

  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "Set quality response: %s", resp);
    return esp_mcp_value_create_bool(true);
  }
  return esp_mcp_value_create_bool(false);
}

static esp_mcp_value_t
mcp_tool_camera_set_fps(const esp_mcp_property_list_t *properties) {
  int fps = esp_mcp_property_list_get_property_int(properties, "fps");
  ESP_LOGI(TAG, "[MCP] mipi_dsi.camera.set_fps: %d", fps);

  char body[32];
  snprintf(body, sizeof(body), "{\"fps\":%d}", fps);

  char resp[64] = {0};
  esp_err_t ret = http_post("/api/camera/fps", body, resp, sizeof(resp));

  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "Set fps response: %s", resp);
    return esp_mcp_value_create_bool(true);
  }
  return esp_mcp_value_create_bool(false);
}

/*---------------------------------------------------------------
 * MCP tool callbacks - Display control
 *-------------------------------------------------------------*/
static esp_mcp_value_t
mcp_tool_display_on(const esp_mcp_property_list_t *properties) {
  (void)properties;
  ESP_LOGI(TAG, "[MCP] mipi_dsi.display.on");

  char resp[64] = {0};
  esp_err_t ret = http_post("/api/display/on", NULL, resp, sizeof(resp));

  if (ret == ESP_OK) {
    return esp_mcp_value_create_bool(true);
  }
  return esp_mcp_value_create_bool(false);
}

static esp_mcp_value_t
mcp_tool_display_off(const esp_mcp_property_list_t *properties) {
  (void)properties;
  ESP_LOGI(TAG, "[MCP] mipi_dsi.display.off");

  char resp[64] = {0};
  esp_err_t ret = http_post("/api/display/off", NULL, resp, sizeof(resp));

  if (ret == ESP_OK) {
    return esp_mcp_value_create_bool(true);
  }
  return esp_mcp_value_create_bool(false);
}

static esp_mcp_value_t
mcp_tool_display_set_brightness(const esp_mcp_property_list_t *properties) {
  int brightness =
      esp_mcp_property_list_get_property_int(properties, "brightness");
  ESP_LOGI(TAG, "[MCP] mipi_dsi.display.set_brightness: %d", brightness);

  char body[32];
  snprintf(body, sizeof(body), "{\"level\":%d}", brightness);

  char resp[64] = {0};
  esp_err_t ret =
      http_post("/api/display/brightness", body, resp, sizeof(resp));

  if (ret == ESP_OK) {
    return esp_mcp_value_create_bool(true);
  }
  return esp_mcp_value_create_bool(false);
}

static esp_mcp_value_t
mcp_tool_display_show_camera(const esp_mcp_property_list_t *properties) {
  (void)properties;
  ESP_LOGI(TAG, "[MCP] mipi_dsi.display.show_camera");

  char resp[64] = {0};
  esp_err_t ret =
      http_post("/api/display/camera_preview", NULL, resp, sizeof(resp));

  if (ret == ESP_OK) {
    return esp_mcp_value_create_bool(true);
  }
  return esp_mcp_value_create_bool(false);
}

/*---------------------------------------------------------------
 * MCP tool callbacks - LED control
 *-------------------------------------------------------------*/
static esp_mcp_value_t
mcp_tool_led_on(const esp_mcp_property_list_t *properties) {
  int brightness =
      esp_mcp_property_list_get_property_int(properties, "brightness");
  int color_temp =
      esp_mcp_property_list_get_property_int(properties, "color_temp");

  /* 亮度 0-100 直接透传（0=最暗）。brightness 参数缺失时 SDK 会在参数校验
   * 阶段报错、不会进入本回调，因此不需要"未指定→100"的兜底，0 必须原样下发。 */
  if (brightness < 0) {
    brightness = 0;
  }
  if (brightness > 100) {
    brightness = 100;
  }

  ESP_LOGI(TAG, "[MCP] mipi_dsi.led.on: brightness=%d, color_temp=%d",
           brightness, color_temp);

  char body[64];
  if (color_temp > 0) {
    snprintf(body, sizeof(body), "{\"brightness\":%d,\"color_temp\":%d}",
             brightness, color_temp);
  } else {
    snprintf(body, sizeof(body), "{\"brightness\":%d}", brightness);
  }

  char resp[64] = {0};
  esp_err_t ret = http_post("/api/led/on", body, resp, sizeof(resp));

  if (ret == ESP_OK) {
    return esp_mcp_value_create_bool(true);
  }
  return esp_mcp_value_create_bool(false);
}

static esp_mcp_value_t
mcp_tool_led_off(const esp_mcp_property_list_t *properties) {
  (void)properties;
  ESP_LOGI(TAG, "[MCP] mipi_dsi.led.off");

  char resp[64] = {0};
  esp_err_t ret = http_post("/api/led/off", NULL, resp, sizeof(resp));

  if (ret == ESP_OK) {
    return esp_mcp_value_create_bool(true);
  }
  return esp_mcp_value_create_bool(false);
}

static esp_mcp_value_t
mcp_tool_led_set_brightness(const esp_mcp_property_list_t *properties) {
  int brightness =
      esp_mcp_property_list_get_property_int(properties, "brightness");

  /* 亮度 0-100 直接透传（0=最暗）。brightness 参数缺失时 SDK 会在参数校验
   * 阶段报错、不会进入本回调，因此不需要"未指定→100"的兜底，0 必须原样下发。 */
  if (brightness < 0) {
    brightness = 0;
  }
  if (brightness > 100) {
    brightness = 100;
  }

  ESP_LOGI(TAG, "[MCP] mipi_dsi.led.set_brightness: %d", brightness);

  char body[32];
  snprintf(body, sizeof(body), "{\"brightness\":%d}", brightness);

  char resp[64] = {0};
  esp_err_t ret = http_post("/api/led/brightness", body, resp, sizeof(resp));

  if (ret == ESP_OK) {
    return esp_mcp_value_create_bool(true);
  }
  return esp_mcp_value_create_bool(false);
}

static esp_mcp_value_t
mcp_tool_led_set_level(const esp_mcp_property_list_t *properties) {
  /* 灯头LED 5档亮度，与灯头板触摸双击调光档位一致 */
  static const uint8_t s_brightness_levels[5] = {3, 7, 12, 16, 20};

  int level = esp_mcp_property_list_get_property_int(properties, "level");

  /* 档位越界钳制（SDK 的 range 约束已保证 1-5，这里做防御） */
  if (level < 1) {
    level = 1;
  }
  if (level > 5) {
    level = 5;
  }

  int brightness = s_brightness_levels[level - 1];

  ESP_LOGI(TAG, "[MCP] mipi_dsi.led.set_level: level=%d -> brightness=%d%%",
           level, brightness);

  char body[32];
  snprintf(body, sizeof(body), "{\"brightness\":%d}", brightness);

  char resp[64] = {0};
  esp_err_t ret = http_post("/api/led/brightness", body, resp, sizeof(resp));

  if (ret == ESP_OK) {
    return esp_mcp_value_create_bool(true);
  }
  return esp_mcp_value_create_bool(false);
}

static esp_mcp_value_t
mcp_tool_led_set_color_temp(const esp_mcp_property_list_t *properties) {
  int color_temp =
      esp_mcp_property_list_get_property_int(properties, "color_temp");

  /* Default to 50 (neutral) if not specified */
  if (color_temp <= 0) {
    color_temp = 50;
  }

  ESP_LOGI(TAG, "[MCP] mipi_dsi.led.set_color_temp: %d", color_temp);

  char body[32];
  snprintf(body, sizeof(body), "{\"color_temp\":%d}", color_temp);

  char resp[64] = {0};
  esp_err_t ret = http_post("/api/led/color_temp", body, resp, sizeof(resp));

  if (ret == ESP_OK) {
    return esp_mcp_value_create_bool(true);
  }
  return esp_mcp_value_create_bool(false);
}

/*---------------------------------------------------------------
 * MCP tool callbacks - Touch control
 *-------------------------------------------------------------*/
static esp_mcp_value_t
mcp_tool_touch_wake(const esp_mcp_property_list_t *properties) {
  (void)properties;
  ESP_LOGI(TAG, "[MCP] mipi_dsi.touch.wake");

  char resp[128] = {0};
  esp_err_t ret = http_get("/api/touch/wake", resp, sizeof(resp), true);

  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "Touch wake response: %s", resp);
    cJSON *root = cJSON_ParseWithOpts(resp, NULL, true);
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    const cJSON *pending = cJSON_GetObjectItemCaseSensitive(data, "wake_pending");
    if (!cJSON_IsObject(root) || !cJSON_IsNumber(code) || code->valuedouble != 0 ||
        !cJSON_IsObject(data) || !cJSON_IsBool(pending)) {
      ESP_LOGW(TAG, "Invalid touch wake response JSON or wake_pending field");
      cJSON_Delete(root);
      return esp_mcp_value_create_bool(false);
    }
    bool wake_pending = cJSON_IsTrue(pending);
    cJSON_Delete(root);
    return esp_mcp_value_create_bool(wake_pending);
  }
  return esp_mcp_value_create_bool(false);
}

static esp_mcp_value_t
mcp_tool_touch_brightness(const esp_mcp_property_list_t *properties) {
  (void)properties;
  ESP_LOGI(TAG, "[MCP] mipi_dsi.touch.brightness");

  char resp[128] = {0};
  esp_err_t ret = http_get("/api/touch/brightness", resp, sizeof(resp), false);

  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "Touch brightness response: %s", resp);
    return esp_mcp_value_create_bool(true);
  }
  return esp_mcp_value_create_bool(false);
}

static esp_mcp_value_t
mcp_tool_touch_reset(const esp_mcp_property_list_t *properties) {
  (void)properties;
  ESP_LOGI(TAG, "[MCP] mipi_dsi.touch.reset");

  char resp[64] = {0};
  esp_err_t ret = http_post("/api/touch/reset", NULL, resp, sizeof(resp));

  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "Touch reset response: %s", resp);
    return esp_mcp_value_create_bool(true);
  }
  return esp_mcp_value_create_bool(false);
}

/*---------------------------------------------------------------
 * MCP tool callbacks - Expressive Eyes control
 *-------------------------------------------------------------*/
static esp_mcp_value_t
mcp_tool_eyes_set_expression(const esp_mcp_property_list_t *properties) {
  const char *expression =
      esp_mcp_property_list_get_property_string(properties, "expression");
  if (!expression || expression[0] == '\0') {
    ESP_LOGW(TAG, "[MCP] mipi_dsi.eyes.set_expression: missing expression");
    return esp_mcp_value_create_bool(false);
  }

  ESP_LOGI(TAG, "[MCP] mipi_dsi.eyes.set_expression: %s", expression);

  char body[64];
  snprintf(body, sizeof(body), "{\"expression\":\"%s\"}", expression);

  char resp[64] = {0};
  esp_err_t ret = http_post("/api/eyes/expression", body, resp, sizeof(resp));

  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "Set expression response: %s", resp);
    return esp_mcp_value_create_bool(true);
  }
  return esp_mcp_value_create_bool(false);
}

static esp_mcp_value_t
mcp_tool_eyes_look_at(const esp_mcp_property_list_t *properties) {
  int x_int = esp_mcp_property_list_get_property_int(properties, "x");
  int y_int = esp_mcp_property_list_get_property_int(properties, "y");

  /* Properties come as integers (e.g. x=50 means 0.5), normalize to [-100, 100]
   * range */
  float x = (float)x_int / 100.0f;
  float y = (float)y_int / 100.0f;

  if (x < -1.0f)
    x = -1.0f;
  if (x > 1.0f)
    x = 1.0f;
  if (y < -1.0f)
    y = -1.0f;
  if (y > 1.0f)
    y = 1.0f;

  ESP_LOGI(TAG, "[MCP] mipi_dsi.eyes.look_at: (%.2f, %.2f)", x, y);

  char body[64];
  snprintf(body, sizeof(body), "{\"x\":%.2f,\"y\":%.2f}", x, y);

  char resp[64] = {0};
  esp_err_t ret = http_post("/api/eyes/look_at", body, resp, sizeof(resp));

  if (ret == ESP_OK) {
    return esp_mcp_value_create_bool(true);
  }
  return esp_mcp_value_create_bool(false);
}

static esp_mcp_value_t
mcp_tool_eyes_blink(const esp_mcp_property_list_t *properties) {
  (void)properties;
  ESP_LOGI(TAG, "[MCP] mipi_dsi.eyes.blink");

  char resp[64] = {0};
  esp_err_t ret = http_post("/api/eyes/blink", NULL, resp, sizeof(resp));

  if (ret == ESP_OK) {
    return esp_mcp_value_create_bool(true);
  }
  return esp_mcp_value_create_bool(false);
}

static esp_mcp_value_t
mcp_tool_eyes_get_expression(const esp_mcp_property_list_t *properties) {
  (void)properties;
  ESP_LOGI(TAG, "[MCP] mipi_dsi.eyes.get_expression");

  char resp[128] = {0};
  esp_err_t ret = http_get("/api/eyes/expression", resp, sizeof(resp), true);

  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "Get expression response: %s", resp);
    cJSON *root = cJSON_ParseWithOpts(resp, NULL, true);
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    const cJSON *expression = cJSON_GetObjectItemCaseSensitive(data, "expression");
    if (!cJSON_IsObject(root) || !cJSON_IsNumber(code) || code->valuedouble != 0 ||
        !cJSON_IsObject(data) || !cJSON_IsString(expression) ||
        !expression->valuestring || !expression->valuestring[0]) {
      ESP_LOGW(TAG, "Invalid eyes expression response JSON or expression field");
      cJSON_Delete(root);
      return esp_mcp_value_create_string("");
    }
    esp_mcp_value_t result = esp_mcp_value_create_string(expression->valuestring);
    cJSON_Delete(root);
    return result;
  }
  return esp_mcp_value_create_string("");
}

/*---------------------------------------------------------------
 * MCP tool registration
 *-------------------------------------------------------------*/
#if (MIPI_DSI_BRIDGE_ENABLE == 1)

esp_err_t mipi_dsi_bridge_register_mcp_tools(esp_mcp_t *mcp) {
  ESP_RETURN_ON_FALSE(mcp, ESP_ERR_INVALID_ARG, TAG, "Invalid MCP engine");

  /* self.mipi_dsi.camera.start */
  esp_mcp_tool_t *cam_start =
      esp_mcp_tool_create("self.mipi_dsi.camera.start",
                          "启动灯头摄像头视频流", mcp_tool_camera_start);
  if (!cam_start) {
    return ESP_ERR_NO_MEM;
  }
  esp_mcp_add_tool(mcp, cam_start);

  /* self.mipi_dsi.camera.stop */
  esp_mcp_tool_t *cam_stop =
      esp_mcp_tool_create("self.mipi_dsi.camera.stop",
                          "停止灯头摄像头视频流", mcp_tool_camera_stop);
  if (!cam_stop) {
    return ESP_ERR_NO_MEM;
  }
  esp_mcp_add_tool(mcp, cam_stop);

  /* self.mipi_dsi.camera.set_quality */
  esp_mcp_tool_t *cam_quality = esp_mcp_tool_create(
      "self.mipi_dsi.camera.set_quality", "设置灯头摄像头JPEG质量 (1-100)",
      mcp_tool_camera_set_quality);
  if (!cam_quality) {
    return ESP_ERR_NO_MEM;
  }
  esp_mcp_property_t *q_prop =
      esp_mcp_property_create_with_range("quality", 1, 100);
  esp_mcp_tool_add_property(cam_quality, q_prop);
  esp_mcp_add_tool(mcp, cam_quality);

  /* self.mipi_dsi.camera.set_fps */
  esp_mcp_tool_t *cam_fps = esp_mcp_tool_create("self.mipi_dsi.camera.set_fps",
                                                "设置灯头摄像头帧率 (1-30)",
                                                mcp_tool_camera_set_fps);
  if (!cam_fps) {
    return ESP_ERR_NO_MEM;
  }
  esp_mcp_property_t *fps_prop =
      esp_mcp_property_create_with_range("fps", 1, 30);
  esp_mcp_tool_add_property(cam_fps, fps_prop);
  esp_mcp_add_tool(mcp, cam_fps);

  /* self.mipi_dsi.display.on */
  esp_mcp_tool_t *disp_on = esp_mcp_tool_create(
      "self.mipi_dsi.display.on", "开启灯头显示屏", mcp_tool_display_on);
  if (!disp_on) {
    return ESP_ERR_NO_MEM;
  }
  esp_mcp_add_tool(mcp, disp_on);

  /* self.mipi_dsi.display.off */
  esp_mcp_tool_t *disp_off = esp_mcp_tool_create(
      "self.mipi_dsi.display.off", "关闭灯头显示屏", mcp_tool_display_off);
  if (!disp_off) {
    return ESP_ERR_NO_MEM;
  }
  esp_mcp_add_tool(mcp, disp_off);

  /* self.mipi_dsi.display.set_brightness */
  esp_mcp_tool_t *disp_bright = esp_mcp_tool_create(
      "self.mipi_dsi.display.set_brightness", "设置灯头显示屏亮度 (0-100)",
      mcp_tool_display_set_brightness);
  if (!disp_bright) {
    return ESP_ERR_NO_MEM;
  }
  esp_mcp_property_t *b_prop =
      esp_mcp_property_create_with_range("brightness", 0, 100);
  esp_mcp_tool_add_property(disp_bright, b_prop);
  esp_mcp_add_tool(mcp, disp_bright);

  /* self.mipi_dsi.display.show_camera */
  esp_mcp_tool_t *disp_cam = esp_mcp_tool_create(
      "self.mipi_dsi.display.show_camera", "在灯头显示屏上显示摄像头预览",
      mcp_tool_display_show_camera);
  if (!disp_cam) {
    return ESP_ERR_NO_MEM;
  }
  esp_mcp_add_tool(mcp, disp_cam);

  /* self.mipi_dsi.led.on */
  esp_mcp_tool_t *led_on = esp_mcp_tool_create(
      "self.mipi_dsi.led.on", "开启灯头板载LED (可指定亮度和色温)",
      mcp_tool_led_on);
  if (!led_on) {
    return ESP_ERR_NO_MEM;
  }
  esp_mcp_property_t *led_bright_prop =
      esp_mcp_property_create_with_range("brightness", 0, 100);
  esp_mcp_tool_add_property(led_on, led_bright_prop);
  esp_mcp_property_t *led_ct_prop =
      esp_mcp_property_create_with_range("color_temp", 0, 100);
  esp_mcp_tool_add_property(led_on, led_ct_prop);
  esp_mcp_add_tool(mcp, led_on);

  /* self.mipi_dsi.led.off */
  esp_mcp_tool_t *led_off = esp_mcp_tool_create(
      "self.mipi_dsi.led.off", "关闭灯头板载LED", mcp_tool_led_off);
  if (!led_off) {
    return ESP_ERR_NO_MEM;
  }
  esp_mcp_add_tool(mcp, led_off);

  /* self.mipi_dsi.led.set_brightness */
  esp_mcp_tool_t *led_set_bright = esp_mcp_tool_create(
      "self.mipi_dsi.led.set_brightness", "设置灯头板载LED亮度 (0-100)",
      mcp_tool_led_set_brightness);
  if (!led_set_bright) {
    return ESP_ERR_NO_MEM;
  }
  esp_mcp_property_t *led_sb_prop =
      esp_mcp_property_create_with_range("brightness", 0, 100);
  esp_mcp_tool_add_property(led_set_bright, led_sb_prop);
  esp_mcp_add_tool(mcp, led_set_bright);

  /* self.mipi_dsi.led.set_level */
  esp_mcp_tool_t *led_set_level = esp_mcp_tool_create(
      "self.mipi_dsi.led.set_level",
      "设置灯头板载LED亮度档位 (level 1-5)。level=1 最低亮度(3%), "
      "level=2(7%), level=3(12%), level=4(16%), level=5 最亮(20%)。"
      "用户说\"最低亮度/最暗/亮度调到最低\"时调用 level=1；"
      "说\"最亮/最高亮度/亮度调到最高\"时调用 level=5",
      mcp_tool_led_set_level);
  if (!led_set_level) {
    return ESP_ERR_NO_MEM;
  }
  esp_mcp_property_t *led_sl_prop =
      esp_mcp_property_create_with_range("level", 1, 5);
  esp_mcp_tool_add_property(led_set_level, led_sl_prop);
  esp_mcp_add_tool(mcp, led_set_level);

  /* self.mipi_dsi.led.set_color_temp */
  esp_mcp_tool_t *led_set_ct =
      esp_mcp_tool_create("self.mipi_dsi.led.set_color_temp",
                          "设置灯头板载LED色温 (0=最暖, 100=最冷)",
                          mcp_tool_led_set_color_temp);
  if (!led_set_ct) {
    return ESP_ERR_NO_MEM;
  }
  esp_mcp_property_t *led_sct_prop =
      esp_mcp_property_create_with_range("color_temp", 0, 100);
  esp_mcp_tool_add_property(led_set_ct, led_sct_prop);
  esp_mcp_add_tool(mcp, led_set_ct);

  /* self.mipi_dsi.touch.wake */
  esp_mcp_tool_t *touch_wake = esp_mcp_tool_create(
      "self.mipi_dsi.touch.wake",
      "查询灯头触摸唤醒事件（轮询方式，有唤醒返回true）",
      mcp_tool_touch_wake);
  if (!touch_wake) {
    return ESP_ERR_NO_MEM;
  }
  esp_mcp_add_tool(mcp, touch_wake);

  /* self.mipi_dsi.touch.brightness */
  esp_mcp_tool_t *touch_bright = esp_mcp_tool_create(
      "self.mipi_dsi.touch.brightness", "查询灯头触摸亮度档位信息",
      mcp_tool_touch_brightness);
  if (!touch_bright) {
    return ESP_ERR_NO_MEM;
  }
  esp_mcp_add_tool(mcp, touch_bright);

  /* self.mipi_dsi.touch.reset */
  esp_mcp_tool_t *touch_reset =
      esp_mcp_tool_create("self.mipi_dsi.touch.reset",
                          "重置灯头触摸配置为默认值", mcp_tool_touch_reset);
  if (!touch_reset) {
    return ESP_ERR_NO_MEM;
  }
  esp_mcp_add_tool(mcp, touch_reset);

  /* self.mipi_dsi.eyes.set_expression */
  esp_mcp_tool_t *eyes_expr = esp_mcp_tool_create(
      "self.mipi_dsi.eyes.set_expression",
      "设置灯头表情眼睛的表情 "
      "(neutral/happy/sad/angry/surprised/sleepy/bored/wink_left/wink_right)",
      mcp_tool_eyes_set_expression);
  if (!eyes_expr) {
    return ESP_ERR_NO_MEM;
  }
  esp_mcp_property_t *expr_prop =
      esp_mcp_property_create("expression", ESP_MCP_PROPERTY_TYPE_STRING);
  esp_mcp_tool_add_property(eyes_expr, expr_prop);
  esp_mcp_add_tool(mcp, eyes_expr);

  /* self.mipi_dsi.eyes.look_at */
  esp_mcp_tool_t *eyes_look = esp_mcp_tool_create(
      "self.mipi_dsi.eyes.look_at",
      "设置灯头表情眼睛的注视方向 (x: -100~100, y: -100~100)",
      mcp_tool_eyes_look_at);
  if (!eyes_look) {
    return ESP_ERR_NO_MEM;
  }
  esp_mcp_property_t *x_prop =
      esp_mcp_property_create_with_range("x", -100, 100);
  esp_mcp_tool_add_property(eyes_look, x_prop);
  esp_mcp_property_t *y_prop =
      esp_mcp_property_create_with_range("y", -100, 100);
  esp_mcp_tool_add_property(eyes_look, y_prop);
  esp_mcp_add_tool(mcp, eyes_look);

  /* self.mipi_dsi.eyes.blink */
  esp_mcp_tool_t *eyes_blink =
      esp_mcp_tool_create("self.mipi_dsi.eyes.blink",
                          "触发灯头表情眼睛眨眼", mcp_tool_eyes_blink);
  if (!eyes_blink) {
    return ESP_ERR_NO_MEM;
  }
  esp_mcp_add_tool(mcp, eyes_blink);

  /* self.mipi_dsi.eyes.get_expression */
  esp_mcp_tool_t *eyes_get = esp_mcp_tool_create(
      "self.mipi_dsi.eyes.get_expression", "获取灯头表情眼睛当前表情",
      mcp_tool_eyes_get_expression);
  if (!eyes_get) {
    return ESP_ERR_NO_MEM;
  }
  esp_mcp_add_tool(mcp, eyes_get);

  ESP_LOGI(TAG, "MIPI-DSI bridge MCP tools registered (19 tools)");
  return ESP_OK;
}

/*---------------------------------------------------------------
 * Init / Deinit
 *-------------------------------------------------------------*/

esp_err_t mipi_dsi_bridge_init(void) {
  ESP_RETURN_ON_FALSE(!s_initialized, ESP_ERR_INVALID_STATE, TAG,
                      "Already initialized");

  s_initialized = true;
  ESP_LOGI(TAG, "MIPI-DSI bridge initialized; target resolved by discovery (role=head)");
  return ESP_OK;
}

void mipi_dsi_bridge_deinit(void) {
  if (!s_initialized) {
    return;
  }

  s_initialized = false;
  ESP_LOGI(TAG, "MIPI-DSI bridge deinitialized");
}

esp_err_t mipi_dsi_bridge_ping(void) {
  ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG,
                      "Not initialized");

  char resp[128] = {0};
  esp_err_t ret = http_get("/api/status", resp, sizeof(resp), false);

  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "Ping successful, status: %s", resp);
  } else {
    ESP_LOGW(TAG, "Ping failed: %s", esp_err_to_name(ret));
  }
  return ret;
}

esp_err_t mipi_dsi_bridge_mode_set(const char *mode) {
  ESP_RETURN_ON_FALSE(mode, ESP_ERR_INVALID_ARG, TAG, "Invalid mode");

  char body[32];
  snprintf(body, sizeof(body), "{\"mode\":\"%s\"}", mode);
  char resp[64] = {0};
  esp_err_t ret = http_post("/api/mode/set", body, resp, sizeof(resp));

  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "Mode sync to head board: %s", mode);
  } else {
    ESP_LOGW(TAG, "Mode sync \"%s\" failed: %s", mode, esp_err_to_name(ret));
  }
  return ret;
}

#else /* MIPI_DSI_BRIDGE_ENABLE == 0 */

/* Stub implementations when component is disabled */

esp_err_t mipi_dsi_bridge_init(void) { return ESP_ERR_NOT_SUPPORTED; }
void mipi_dsi_bridge_deinit(void) {}
esp_err_t mipi_dsi_bridge_register_mcp_tools(esp_mcp_t *mcp) {
  (void)mcp;
  return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t mipi_dsi_bridge_ping(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t mipi_dsi_bridge_mode_set(const char *mode) {
  (void)mode;
  return ESP_ERR_NOT_SUPPORTED;
}

#endif /* MIPI_DSI_BRIDGE_ENABLE */
