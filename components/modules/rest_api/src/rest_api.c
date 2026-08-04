/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rest_api.h"
#include "rest_api_config.h"

#if (REST_API_ENABLE == 1)

#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"

#include "cJSON.h"

#include "status_reporter.h"
#include "system_config.h"
#include "websocket_manager.h"

/* Hardware service headers */
#include "lcd_driver.h"
#include "lcd_module.h"
#include "led_service.h"
#include "servo_service.h"

/* App headers */
#include "focus_app.h"
#include "motion_controller.h"

static const char *TAG = "REST_API";

#define REST_JSON_BUF_SIZE 512
#define REST_BODY_BUF_SIZE 512
#define REST_SERVO_DEFAULT_TIME_MS 1000

/*---------------------------------------------------------------
 * CRC16-CCITT for response integrity verification
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
 * Helper: send JSON response with CRC16 header
 *-------------------------------------------------------------*/
static esp_err_t send_json_response(httpd_req_t *req, const char *json_str) {
  /* Set Content-Type */
  httpd_resp_set_type(req, "application/json");

  /* Compute CRC16 of response body and set header */
  uint16_t crc = crc16_ccitt((const uint8_t *)json_str, strlen(json_str));
  char crc_hdr[8];
  snprintf(crc_hdr, sizeof(crc_hdr), "%04X", crc);
  httpd_resp_set_hdr(req, "X-CRC16", crc_hdr);

  /* Send response */
  return httpd_resp_send(req, json_str, strlen(json_str));
}

/*---------------------------------------------------------------
 * Helper: send JSON response with custom HTTP status code
 *-------------------------------------------------------------*/
static esp_err_t send_json_response_status(httpd_req_t *req,
                                           const char *json_str,
                                           const char *status_code) {
  httpd_resp_set_status(req, status_code);
  return send_json_response(req, json_str);
}

/*---------------------------------------------------------------
 * Helper: send 400 error response
 *-------------------------------------------------------------*/
static esp_err_t send_error_400(httpd_req_t *req, const char *message) {
  char json[128];
  snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}",
           message ? message : "bad request");
  return send_json_response_status(req, json, "400 Bad Request");
}

/*---------------------------------------------------------------
 * Helper: send 500 error response
 *-------------------------------------------------------------*/
static esp_err_t send_error_500(httpd_req_t *req, const char *message) {
  char json[128];
  snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}",
           message ? message : "internal error");
  return send_json_response_status(req, json, "500 Internal Server Error");
}

/*---------------------------------------------------------------
 * Helper: read POST body from request
 * Returns 0 on success, -1 on failure
 *-------------------------------------------------------------*/
static int read_post_body(httpd_req_t *req, char *buf, size_t buf_size) {
  size_t total = req->content_len;
  if (total == 0) {
    buf[0] = '\0';
    return 0;
  }
  if (total >= buf_size) {
    /* Truncate to fit */
    total = buf_size - 1;
  }

  int received = 0;
  while ((size_t)received < total) {
    int ret = httpd_req_recv(req, buf + received, total - received);
    if (ret <= 0) {
      if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
        continue;
      }
      return -1;
    }
    received += ret;
  }
  buf[received] = '\0';
  return 0;
}

/*---------------------------------------------------------------
 * Helper: get WiFi IP string
 *-------------------------------------------------------------*/
static void get_ip_string(char *buf, size_t buf_len) {
  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (netif == NULL) {
    snprintf(buf, buf_len, "0.0.0.0");
    return;
  }
  esp_netif_ip_info_t ip_info;
  if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
    snprintf(buf, buf_len, IPSTR, IP2STR(&ip_info.ip));
  } else {
    snprintf(buf, buf_len, "0.0.0.0");
  }
}

/*---------------------------------------------------------------
 * Handler: GET /api/status
 *
 * Returns device status JSON aligned with mipi_dsi status format:
 * {
 *   "device": "focuslamp",
 *   "version": "1.0.0",
 *   "ip": "x.x.x.x",
 *   "uptime": 12345,
 *   "wifi_rssi": -65,
 *   "free_heap": 123456
 * }
 *-------------------------------------------------------------*/
static esp_err_t status_get_handler(httpd_req_t *req) {
  char ip_str[16] = {0};
  get_ip_string(ip_str, sizeof(ip_str));

  int64_t uptime_sec = esp_timer_get_time() / 1000000;
  int free_heap = (int)esp_get_free_heap_size();

  /* WiFi RSSI */
  int rssi = 0;
  wifi_ap_record_t ap_info;
  if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
    rssi = ap_info.rssi;
  }

  char json[256];
  snprintf(json, sizeof(json),
           "{\"device\":\"%s\",\"version\":\"%d.%d.%d\",\"ip\":\"%s\","
           "\"uptime\":%lld,\"wifi_rssi\":%d,\"free_heap\":%d}",
           PROJECT_NAME, SYSTEM_VERSION_MAJOR, SYSTEM_VERSION_MINOR,
           SYSTEM_VERSION_PATCH, ip_str, (long long)uptime_sec, rssi,
           free_heap);

  ESP_LOGI(TAG, "GET /api/status -> %s", json);
  return send_json_response(req, json);
}

static const httpd_uri_t status_uri = {
    .uri = "/api/status",
    .method = HTTP_GET,
    .handler = status_get_handler,
    .user_ctx = NULL,
};

/*---------------------------------------------------------------
 * LED Control Handlers (6 endpoints)
 *-------------------------------------------------------------*/

/* Handler: POST /api/led/on - Turn on LED with optional brightness */
static esp_err_t led_on_handler(httpd_req_t *req) {
  char body[REST_BODY_BUF_SIZE];
  if (read_post_body(req, body, sizeof(body)) != 0) {
    return send_error_500(req, "read body failed");
  }

  int brightness = 255;
  cJSON *root = cJSON_Parse(body);
  if (root) {
    cJSON *b = cJSON_GetObjectItem(root, "brightness");
    if (cJSON_IsNumber(b)) {
      brightness = b->valueint;
    }
    cJSON_Delete(root);
  }

  if (brightness < 0)
    brightness = 0;
  if (brightness > 255)
    brightness = 255;

  led_service_set_brightness((uint8_t)brightness);
  led_service_set_mode(LED_MODE_WHITE);
  led_service_set_effect(LED_EFFECT_STEADY);

  char json[128];
  snprintf(json, sizeof(json), "{\"ok\":true,\"brightness\":%d}", brightness);
  ESP_LOGI(TAG, "POST /api/led/on brightness=%d", brightness);
  status_reporter_notify_event(STATUS_EVENT_LED);
  return send_json_response(req, json);
}

static const httpd_uri_t led_on_uri = {
    .uri = "/api/led/on",
    .method = HTTP_POST,
    .handler = led_on_handler,
    .user_ctx = NULL,
};

/* Handler: POST /api/led/off - Turn off LED */
static esp_err_t led_off_handler(httpd_req_t *req) {
  led_service_turn_off();
  ESP_LOGI(TAG, "POST /api/led/off");
  status_reporter_notify_event(STATUS_EVENT_LED);
  return send_json_response(req, "{\"ok\":true}");
}

static const httpd_uri_t led_off_uri = {
    .uri = "/api/led/off",
    .method = HTTP_POST,
    .handler = led_off_handler,
    .user_ctx = NULL,
};

/* Handler: POST /api/led/brightness - Set brightness */
static esp_err_t led_brightness_handler(httpd_req_t *req) {
  char body[REST_BODY_BUF_SIZE];
  if (read_post_body(req, body, sizeof(body)) != 0) {
    return send_error_500(req, "read body failed");
  }

  cJSON *root = cJSON_Parse(body);
  if (!root) {
    return send_error_400(req, "invalid JSON");
  }
  cJSON *b = cJSON_GetObjectItem(root, "brightness");
  if (!cJSON_IsNumber(b)) {
    cJSON_Delete(root);
    return send_error_400(req, "missing or invalid brightness");
  }

  int brightness = b->valueint;
  cJSON_Delete(root);

  if (brightness < 0)
    brightness = 0;
  if (brightness > 255)
    brightness = 255;

  esp_err_t ret = led_service_set_brightness((uint8_t)brightness);
  if (ret != ESP_OK) {
    return send_error_500(req, "set_brightness failed");
  }

  char json[128];
  snprintf(json, sizeof(json), "{\"ok\":true,\"brightness\":%d}", brightness);
  ESP_LOGI(TAG, "POST /api/led/brightness %d", brightness);
  status_reporter_notify_event(STATUS_EVENT_LED);
  return send_json_response(req, json);
}

static const httpd_uri_t led_brightness_uri = {
    .uri = "/api/led/brightness",
    .method = HTTP_POST,
    .handler = led_brightness_handler,
    .user_ctx = NULL,
};

/* Handler: POST /api/led/effect - Set effect */
static esp_err_t led_effect_handler(httpd_req_t *req) {
  char body[REST_BODY_BUF_SIZE];
  if (read_post_body(req, body, sizeof(body)) != 0) {
    return send_error_500(req, "read body failed");
  }

  cJSON *root = cJSON_Parse(body);
  if (!root) {
    return send_error_400(req, "invalid JSON");
  }
  cJSON *e = cJSON_GetObjectItem(root, "effect");
  if (!cJSON_IsString(e) || !e->valuestring) {
    cJSON_Delete(root);
    return send_error_400(req, "missing or invalid effect");
  }

  led_effect_t effect;
  if (strcmp(e->valuestring, "steady") == 0) {
    effect = LED_EFFECT_STEADY;
  } else if (strcmp(e->valuestring, "breathing") == 0) {
    effect = LED_EFFECT_BREATHING;
  } else if (strcmp(e->valuestring, "rainbow") == 0) {
    effect = LED_EFFECT_RAINBOW;
  } else if (strcmp(e->valuestring, "blinking") == 0) {
    effect = LED_EFFECT_BLINKING;
  } else {
    cJSON_Delete(root);
    return send_error_400(req, "unknown effect");
  }

  const char *effect_str = e->valuestring;
  cJSON_Delete(root);

  esp_err_t ret = led_service_set_effect(effect);
  if (ret != ESP_OK) {
    return send_error_500(req, "set_effect failed");
  }

  char json[128];
  snprintf(json, sizeof(json), "{\"ok\":true,\"effect\":\"%s\"}", effect_str);
  ESP_LOGI(TAG, "POST /api/led/effect %s", effect_str);
  status_reporter_notify_event(STATUS_EVENT_LED);
  return send_json_response(req, json);
}

static const httpd_uri_t led_effect_uri = {
    .uri = "/api/led/effect",
    .method = HTTP_POST,
    .handler = led_effect_handler,
    .user_ctx = NULL,
};

/* Handler: POST /api/led/color - Set color */
static esp_err_t led_color_handler(httpd_req_t *req) {
  char body[REST_BODY_BUF_SIZE];
  if (read_post_body(req, body, sizeof(body)) != 0) {
    return send_error_500(req, "read body failed");
  }

  cJSON *root = cJSON_Parse(body);
  if (!root) {
    return send_error_400(req, "invalid JSON");
  }
  cJSON *r = cJSON_GetObjectItem(root, "r");
  cJSON *g = cJSON_GetObjectItem(root, "g");
  cJSON *b = cJSON_GetObjectItem(root, "b");
  if (!cJSON_IsNumber(r) || !cJSON_IsNumber(g) || !cJSON_IsNumber(b)) {
    cJSON_Delete(root);
    return send_error_400(req, "missing or invalid r/g/b values");
  }

  uint8_t red = (uint8_t)(r->valueint & 0xFF);
  uint8_t green = (uint8_t)(g->valueint & 0xFF);
  uint8_t blue = (uint8_t)(b->valueint & 0xFF);
  cJSON_Delete(root);

  led_service_set_mode(LED_MODE_COLOR);
  esp_err_t ret = led_service_set_color(red, green, blue);
  if (ret != ESP_OK) {
    return send_error_500(req, "set_color failed");
  }

  char json[128];
  snprintf(json, sizeof(json),
           "{\"ok\":true,\"color\":{\"r\":%d,\"g\":%d,\"b\":%d}}", red, green,
           blue);
  ESP_LOGI(TAG, "POST /api/led/color (%d,%d,%d)", red, green, blue);
  status_reporter_notify_event(STATUS_EVENT_LED);
  return send_json_response(req, json);
}

static const httpd_uri_t led_color_uri = {
    .uri = "/api/led/color",
    .method = HTTP_POST,
    .handler = led_color_handler,
    .user_ctx = NULL,
};

/* Handler: GET /api/led/status - Get LED status */
static esp_err_t led_status_handler(httpd_req_t *req) {
  led_module_snapshot_t snap;
  esp_err_t ret = led_service_get_snapshot(&snap);
  if (ret != ESP_OK) {
    return send_error_500(req, "snapshot failed");
  }

  bool is_on = (snap.mode != LED_MODULE_MODE_OFF) && snap.initialized;
  char json[256];
  snprintf(json, sizeof(json),
           "{\"ok\":true,\"brightness\":%d,\"mode\":%d,"
           "\"color\":{\"r\":%d,\"g\":%d,\"b\":%d},"
           "\"is_on\":%s,\"brightness_level\":%d}",
           snap.brightness, (int)snap.mode, snap.color.red, snap.color.green,
           snap.color.blue, is_on ? "true" : "false", snap.brightness_level);

  ESP_LOGI(TAG, "GET /api/led/status -> %s", json);
  return send_json_response(req, json);
}

static const httpd_uri_t led_status_uri = {
    .uri = "/api/led/status",
    .method = HTTP_GET,
    .handler = led_status_handler,
    .user_ctx = NULL,
};

/*---------------------------------------------------------------
 * Display Control Handlers (3 endpoints)
 *-------------------------------------------------------------*/

/* Handler: POST /api/display/on - Turn on display */
static esp_err_t display_on_handler(httpd_req_t *req) {
  esp_err_t ret = lcd_driver_display_on();
  ESP_LOGI(TAG, "POST /api/display/on -> %s", esp_err_to_name(ret));
  if (ret != ESP_OK) {
    return send_error_500(req, "display_on failed");
  }
  status_reporter_notify_event(STATUS_EVENT_DISPLAY);
  return send_json_response(req, "{\"ok\":true}");
}

static const httpd_uri_t display_on_uri = {
    .uri = "/api/display/on",
    .method = HTTP_POST,
    .handler = display_on_handler,
    .user_ctx = NULL,
};

/* Handler: POST /api/display/off - Turn off display */
static esp_err_t display_off_handler(httpd_req_t *req) {
  esp_err_t ret = lcd_driver_display_off();
  ESP_LOGI(TAG, "POST /api/display/off -> %s", esp_err_to_name(ret));
  if (ret != ESP_OK) {
    return send_error_500(req, "display_off failed");
  }
  status_reporter_notify_event(STATUS_EVENT_DISPLAY);
  return send_json_response(req, "{\"ok\":true}");
}

static const httpd_uri_t display_off_uri = {
    .uri = "/api/display/off",
    .method = HTTP_POST,
    .handler = display_off_handler,
    .user_ctx = NULL,
};

/* Handler: POST /api/display/brightness - Set display brightness */
static esp_err_t display_brightness_handler(httpd_req_t *req) {
  char body[REST_BODY_BUF_SIZE];
  if (read_post_body(req, body, sizeof(body)) != 0) {
    return send_error_500(req, "read body failed");
  }

  cJSON *root = cJSON_Parse(body);
  if (!root) {
    return send_error_400(req, "invalid JSON");
  }
  cJSON *l = cJSON_GetObjectItem(root, "level");
  if (!cJSON_IsNumber(l)) {
    cJSON_Delete(root);
    return send_error_400(req, "missing or invalid level");
  }

  int level = l->valueint;
  cJSON_Delete(root);

  if (level < 0)
    level = 0;
  if (level > 255)
    level = 255;

  lcd_driver_set_backlight((uint8_t)level);

  char json[128];
  snprintf(json, sizeof(json), "{\"ok\":true,\"level\":%d}", level);
  ESP_LOGI(TAG, "POST /api/display/brightness %d", level);
  status_reporter_notify_event(STATUS_EVENT_DISPLAY);
  return send_json_response(req, json);
}

static const httpd_uri_t display_brightness_uri = {
    .uri = "/api/display/brightness",
    .method = HTTP_POST,
    .handler = display_brightness_handler,
    .user_ctx = NULL,
};

/*---------------------------------------------------------------
 * LCD Control Handlers (4 endpoints)
 *-------------------------------------------------------------*/

/* Handler: POST /api/lcd/page/next - Switch to next page */
static esp_err_t lcd_page_next_handler(httpd_req_t *req) {
  esp_err_t ret = lcd_module_next_page();
  ESP_LOGI(TAG, "POST /api/lcd/page/next -> %s", esp_err_to_name(ret));
  if (ret != ESP_OK) {
    return send_error_500(req, "next_page failed");
  }
  status_reporter_notify_event(STATUS_EVENT_LCD);
  return send_json_response(req, "{\"ok\":true}");
}

static const httpd_uri_t lcd_page_next_uri = {
    .uri = "/api/lcd/page/next",
    .method = HTTP_POST,
    .handler = lcd_page_next_handler,
    .user_ctx = NULL,
};

/* Handler: POST /api/lcd/expression - Set LCD expression */
static esp_err_t lcd_expression_handler(httpd_req_t *req) {
  static const char *expr_names[] = {"normal", "happy",     "sad",
                                     "angry",  "surprised", "sleepy"};
  const size_t expr_count = sizeof(expr_names) / sizeof(expr_names[0]);

  char body[REST_BODY_BUF_SIZE];
  if (read_post_body(req, body, sizeof(body)) != 0) {
    return send_error_500(req, "read body failed");
  }

  cJSON *root = cJSON_Parse(body);
  if (!root) {
    return send_error_400(req, "invalid JSON");
  }
  cJSON *ex = cJSON_GetObjectItem(root, "expression");
  if (!cJSON_IsString(ex) || !ex->valuestring) {
    cJSON_Delete(root);
    return send_error_400(req, "missing or invalid expression");
  }

  lcd_expression_t expr = LCD_EXPRESSION_NORMAL;
  bool found = false;
  for (size_t i = 0; i < expr_count; i++) {
    if (strcmp(ex->valuestring, expr_names[i]) == 0) {
      expr = (lcd_expression_t)i;
      found = true;
      break;
    }
  }

  const char *expr_str = ex->valuestring;
  cJSON_Delete(root);

  if (!found) {
    return send_error_400(req, "unknown expression");
  }

  esp_err_t ret = lcd_module_set_expression(expr);
  if (ret != ESP_OK) {
    return send_error_500(req, "set_expression failed");
  }

  char json[128];
  snprintf(json, sizeof(json), "{\"ok\":true,\"expression\":\"%s\"}", expr_str);
  ESP_LOGI(TAG, "POST /api/lcd/expression %s", expr_str);
  status_reporter_notify_event(STATUS_EVENT_LCD);
  return send_json_response(req, json);
}

static const httpd_uri_t lcd_expression_uri = {
    .uri = "/api/lcd/expression",
    .method = HTTP_POST,
    .handler = lcd_expression_handler,
    .user_ctx = NULL,
};

/* Handler: POST /api/lcd/blink/enable - Enable auto blink */
static esp_err_t lcd_blink_enable_handler(httpd_req_t *req) {
  lcd_module_set_auto_blink(true);
  ESP_LOGI(TAG, "POST /api/lcd/blink/enable");
  status_reporter_notify_event(STATUS_EVENT_LCD);
  return send_json_response(req, "{\"ok\":true}");
}

static const httpd_uri_t lcd_blink_enable_uri = {
    .uri = "/api/lcd/blink/enable",
    .method = HTTP_POST,
    .handler = lcd_blink_enable_handler,
    .user_ctx = NULL,
};

/* Handler: POST /api/lcd/blink/disable - Disable auto blink */
static esp_err_t lcd_blink_disable_handler(httpd_req_t *req) {
    lcd_module_set_auto_blink(false);
    ESP_LOGI(TAG, "POST /api/lcd/blink/disable");
    status_reporter_notify_event(STATUS_EVENT_LCD);
    return send_json_response(req, "{\"ok\":true}");
}

static const httpd_uri_t lcd_blink_disable_uri = {
    .uri = "/api/lcd/blink/disable",
    .method = HTTP_POST,
    .handler = lcd_blink_disable_handler,
    .user_ctx = NULL,
};

/*---------------------------------------------------------------
 * Servo Control Handlers (3 endpoints)
 *
 * Servo ID mapping: 0=EM3 (range 0-3000), 1-4=LX (range 0-1000).
 * Because servo_service_set_all_positions moves all 5 servos as a
 * group, single-servo set is implemented by reading current positions
 * of all servos, modifying only the target servo, and calling
 * set_all_positions (non-target servos stay put).
 *-------------------------------------------------------------*/

/* Handler: POST /api/servo/position - Set single servo position */
static esp_err_t servo_position_post_handler(httpd_req_t *req) {
  char body[REST_BODY_BUF_SIZE];
  if (read_post_body(req, body, sizeof(body)) != 0) {
    return send_error_500(req, "read body failed");
  }

  cJSON *root = cJSON_Parse(body);
  if (!root) {
    return send_error_400(req, "invalid JSON");
  }
  cJSON *id_item = cJSON_GetObjectItem(root, "servo_id");
  cJSON *pos_item = cJSON_GetObjectItem(root, "position");
  if (!cJSON_IsNumber(id_item) || !cJSON_IsNumber(pos_item)) {
    cJSON_Delete(root);
    return send_error_400(req, "missing or invalid servo_id/position");
  }

  int servo_id = id_item->valueint;
  int position = pos_item->valueint;
  cJSON_Delete(root);

  if (servo_id < 0 || servo_id > 4) {
    return send_error_400(req, "servo_id out of range (0-4)");
  }
  /* Clamp per servo type */
  if (servo_id == 0) {
    if (position < 0)
      position = 0;
    if (position > 3000)
      position = 3000;
  } else {
    if (position < 0)
      position = 0;
    if (position > 1000)
      position = 1000;
  }

  /* Build full positions struct from current servo positions, modify target
   * only */
  servo_service_positions_t target;
  target.em3_pos = (int16_t)servo_service_get_position(0);
  for (int i = 0; i < SERVO_SERVICE_LX_COUNT; i++) {
    target.lx_pos[i] = (int16_t)servo_service_get_position((uint8_t)(i + 1));
  }

  if (servo_id == 0) {
    target.em3_pos = (int16_t)position;
  } else {
    target.lx_pos[servo_id - 1] = (int16_t)position;
  }

  esp_err_t ret =
      servo_service_set_all_positions(&target, REST_SERVO_DEFAULT_TIME_MS);
  if (ret != ESP_OK) {
    return send_error_500(req, "set_all_positions failed");
  }

  char json[128];
  snprintf(json, sizeof(json), "{\"ok\":true,\"servo_id\":%d,\"position\":%d}",
           servo_id, position);
  ESP_LOGI(TAG, "POST /api/servo/position id=%d pos=%d", servo_id, position);
    status_reporter_notify_event(STATUS_EVENT_SERVO);
    return send_json_response(req, json);
}

static const httpd_uri_t servo_position_post_uri = {
    .uri = "/api/servo/position",
    .method = HTTP_POST,
    .handler = servo_position_post_handler,
    .user_ctx = NULL,
};

/* Handler: GET /api/servo/position - Get all servo positions */
static esp_err_t servo_position_get_handler(httpd_req_t *req) {
  int em3 = (int)servo_service_get_position(0);
  int lx0 = (int)servo_service_get_position(1);
  int lx1 = (int)servo_service_get_position(2);
  int lx2 = (int)servo_service_get_position(3);
  int lx3 = (int)servo_service_get_position(4);

  char json[256];
  snprintf(json, sizeof(json), "{\"ok\":true,\"em3\":%d,\"lx\":[%d,%d,%d,%d]}",
           em3, lx0, lx1, lx2, lx3);

  ESP_LOGI(TAG, "GET /api/servo/position -> %s", json);
  return send_json_response(req, json);
}

static const httpd_uri_t servo_position_get_uri = {
    .uri = "/api/servo/position",
    .method = HTTP_GET,
    .handler = servo_position_get_handler,
    .user_ctx = NULL,
};

/* Handler: POST /api/servo/home - Move all servos to home position */
static esp_err_t servo_home_handler(httpd_req_t *req) {
  char body[REST_BODY_BUF_SIZE];
  if (read_post_body(req, body, sizeof(body)) != 0) {
    return send_error_500(req, "read body failed");
  }

  int time_ms = REST_SERVO_DEFAULT_TIME_MS;
  cJSON *root = cJSON_Parse(body);
  if (root) {
    cJSON *t = cJSON_GetObjectItem(root, "time_ms");
    if (cJSON_IsNumber(t)) {
      time_ms = t->valueint;
    }
    cJSON_Delete(root);
  }

  if (time_ms < 0)
    time_ms = 0;
  if (time_ms > 65535)
    time_ms = 65535;

  esp_err_t ret = servo_service_go_home((uint16_t)time_ms);
  if (ret != ESP_OK) {
    return send_error_500(req, "go_home failed");
  }

  char json[128];
  snprintf(json, sizeof(json), "{\"ok\":true,\"time_ms\":%d}", time_ms);
  ESP_LOGI(TAG, "POST /api/servo/home time=%dms", time_ms);
    status_reporter_notify_event(STATUS_EVENT_SERVO);
    return send_json_response(req, json);
}

static const httpd_uri_t servo_home_uri = {
    .uri = "/api/servo/home",
    .method = HTTP_POST,
    .handler = servo_home_handler,
    .user_ctx = NULL,
};

/*---------------------------------------------------------------
 * System Query Handlers (3 endpoints, including existing status)
 *-------------------------------------------------------------*/

/* Handler: GET /api/system/info - Device info */
static esp_err_t system_info_handler(httpd_req_t *req) {
  uint32_t free_heap = esp_get_free_heap_size();
  int64_t uptime_us = esp_timer_get_time();

  char json[256];
  snprintf(json, sizeof(json),
           "{\"ok\":true,\"device\":\"%s\",\"version\":\"%d.%d.%d\","
           "\"free_heap\":%lu,\"uptime_us\":%lld,\"chip\":\"%s\"}",
           PROJECT_NAME, SYSTEM_VERSION_MAJOR, SYSTEM_VERSION_MINOR,
           SYSTEM_VERSION_PATCH, (unsigned long)free_heap, (long long)uptime_us,
           CONFIG_IDF_TARGET);

  ESP_LOGI(TAG, "GET /api/system/info -> %s", json);
  return send_json_response(req, json);
}

static const httpd_uri_t system_info_uri = {
    .uri = "/api/system/info",
    .method = HTTP_GET,
    .handler = system_info_handler,
    .user_ctx = NULL,
};

/* Handler: GET /api/radar/status - Radar status (simplified, no radar
 * dependency) Returns zeroed data to avoid coupling to radar module. */
static esp_err_t radar_status_handler(httpd_req_t *req) {
  const char *json = "{\"ok\":true,\"present\":false,\"heart_rate_bpm\":0,"
                     "\"resp_rate\":0,\"distance_cm\":0}";
  ESP_LOGI(TAG, "GET /api/radar/status -> %s", json);
  return send_json_response(req, json);
}

static const httpd_uri_t radar_status_uri = {
    .uri = "/api/radar/status",
    .method = HTTP_GET,
    .handler = radar_status_handler,
    .user_ctx = NULL,
};

/*---------------------------------------------------------------
 * Focus Mode Handlers (2 endpoints)
 *-------------------------------------------------------------*/

/* Handler: POST /api/focus/start - Start focus timer */
static esp_err_t focus_start_handler(httpd_req_t *req) {
  char body[REST_BODY_BUF_SIZE];
  if (read_post_body(req, body, sizeof(body)) != 0) {
    return send_error_500(req, "read body failed");
  }

  uint32_t duration = 30; /* 默认30分钟 */
  cJSON *root = cJSON_Parse(body);
  if (root) {
    cJSON *d = cJSON_GetObjectItem(root, "duration");
    if (cJSON_IsNumber(d) && d->valueint > 0) {
      duration = (uint32_t)d->valueint;
    }
    cJSON_Delete(root);
  }

  esp_err_t ret = focus_app_start(duration);
  if (ret != ESP_OK) {
    return send_error_500(req, "focus_app_start failed");
  }

  char json[128];
  snprintf(json, sizeof(json), "{\"ok\":true,\"duration\":%lu}", (unsigned long)duration);
  ESP_LOGI(TAG, "POST /api/focus/start duration=%lu min", (unsigned long)duration);
  return send_json_response(req, json);
}

static const httpd_uri_t focus_start_uri = {
    .uri = "/api/focus/start",
    .method = HTTP_POST,
    .handler = focus_start_handler,
    .user_ctx = NULL,
};

/* Handler: POST /api/focus/stop - Stop focus timer */
static esp_err_t focus_stop_handler(httpd_req_t *req) {
  esp_err_t ret = focus_app_stop();
  if (ret != ESP_OK) {
    return send_error_500(req, "focus_app_stop failed");
  }

  ESP_LOGI(TAG, "POST /api/focus/stop");
  return send_json_response(req, "{\"ok\":true}");
}

static const httpd_uri_t focus_stop_uri = {
    .uri = "/api/focus/stop",
    .method = HTTP_POST,
    .handler = focus_stop_handler,
    .user_ctx = NULL,
};

/*---------------------------------------------------------------
 * Motion Control Handlers (6 endpoints)
 *-------------------------------------------------------------*/

/* Generic motion action handler */
static esp_err_t motion_action_handler(httpd_req_t *req, const char *command)
{
    esp_err_t ret = motion_voice_command_execute(command);
    if (ret != ESP_OK) {
        return send_error_500(req, "motion action failed");
    }
    char json[128];
    snprintf(json, sizeof(json), "{\"ok\":true,\"action\":\"%s\"}", command);
    ESP_LOGI(TAG, "POST motion: %s", command);
    return send_json_response(req, json);
}

static esp_err_t motion_wave_handler(httpd_req_t *req)    { return motion_action_handler(req, "wave"); }
static esp_err_t motion_nod_handler(httpd_req_t *req)     { return motion_action_handler(req, "nod"); }
static esp_err_t motion_shake_handler(httpd_req_t *req)   { return motion_action_handler(req, "shake"); }
static esp_err_t motion_dance_handler(httpd_req_t *req)   { return motion_action_handler(req, "dance"); }
static esp_err_t motion_greet_handler(httpd_req_t *req)   { return motion_action_handler(req, "greet"); }
static esp_err_t motion_home_handler(httpd_req_t *req)    { return motion_action_handler(req, "home"); }

static const httpd_uri_t motion_wave_uri  = { .uri = "/api/motion/wave",  .method = HTTP_POST, .handler = motion_wave_handler,  .user_ctx = NULL };
static const httpd_uri_t motion_nod_uri   = { .uri = "/api/motion/nod",   .method = HTTP_POST, .handler = motion_nod_handler,   .user_ctx = NULL };
static const httpd_uri_t motion_shake_uri = { .uri = "/api/motion/shake", .method = HTTP_POST, .handler = motion_shake_handler, .user_ctx = NULL };
static const httpd_uri_t motion_dance_uri = { .uri = "/api/motion/dance", .method = HTTP_POST, .handler = motion_dance_handler, .user_ctx = NULL };
static const httpd_uri_t motion_greet_uri = { .uri = "/api/motion/greet", .method = HTTP_POST, .handler = motion_greet_handler, .user_ctx = NULL };
static const httpd_uri_t motion_home_uri  = { .uri = "/api/motion/home",  .method = HTTP_POST, .handler = motion_home_handler,  .user_ctx = NULL };

/*---------------------------------------------------------------
 * Init / Deinit
 *-------------------------------------------------------------*/
static bool s_initialized = false;

/* Table of all REST URI descriptors for batch registration.
 * Total: 19 endpoints (1 existing status + 18 new). */
static const httpd_uri_t *const s_rest_uris[] = {
    /* System query (3) */
    &status_uri,
    &system_info_uri,
    &radar_status_uri,
    /* LED control (6) */
    &led_on_uri,
    &led_off_uri,
    &led_brightness_uri,
    &led_effect_uri,
    &led_color_uri,
    &led_status_uri,
    /* Display control (3) */
    &display_on_uri,
    &display_off_uri,
    &display_brightness_uri,
    /* LCD control (4) */
    &lcd_page_next_uri,
    &lcd_expression_uri,
    &lcd_blink_enable_uri,
    &lcd_blink_disable_uri,
    /* Servo control (3) */
    &servo_position_post_uri,
    &servo_position_get_uri,
    &servo_home_uri,
    /* Focus mode (2) */
    &focus_start_uri,
    &focus_stop_uri,
    /* Motion control (6) */
    &motion_wave_uri,
    &motion_nod_uri,
    &motion_shake_uri,
    &motion_dance_uri,
    &motion_greet_uri,
    &motion_home_uri,
};
#define REST_URI_COUNT (sizeof(s_rest_uris) / sizeof(s_rest_uris[0]))

esp_err_t rest_api_init(void) {
  if (s_initialized) {
    ESP_LOGW(TAG, "REST API already initialized");
    return ESP_ERR_INVALID_STATE;
  }

  /* Verify websocket_manager server is running */
  if (!ws_manager_server_is_running()) {
    ESP_LOGE(TAG,
             "websocket_manager server not running, cannot register REST URIs");
    return ESP_ERR_INVALID_STATE;
  }

  /* Register all URI handlers */
  int registered = 0;
  for (size_t i = 0; i < REST_URI_COUNT; i++) {
    esp_err_t ret = ws_manager_server_register_uri(s_rest_uris[i]);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "Failed to register %s: %s", s_rest_uris[i]->uri,
               esp_err_to_name(ret));
      return ret;
    }
    registered++;
  }

  s_initialized = true;
  ESP_LOGI(TAG, "REST API initialized, endpoints registered: %d", registered);
  return ESP_OK;
}

void rest_api_deinit(void) {
  if (!s_initialized) {
    return;
  }
  /* Note: esp_http_server does not support URI unregistration,
   * handlers remain registered but s_initialized flag prevents re-init. */
  s_initialized = false;
  ESP_LOGI(TAG, "REST API deinitialized");
}

#else /* REST_API_ENABLE == 0 */

esp_err_t rest_api_init(void) { return ESP_ERR_NOT_SUPPORTED; }
void rest_api_deinit(void) {}

#endif /* REST_API_ENABLE */
