/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"

#include "focuslamp_bridge.h"
#include "focuslamp_bridge_config.h"
#include "led_service.h"
#include "motion_controller.h"
#include "controller_modes.h"
#include "lcd_driver.h"
#include "system_config.h"

#include "esp_mcp_data.h"
#include "esp_mcp_engine.h"
#include "esp_mcp_property.h"
#include "esp_mcp_tool.h"

static const char *TAG = "FOCUSLAMP_BRIDGE";

/* Local controller services; no remote base address or HTTP client. */
static bool s_initialized = false;

/*---------------------------------------------------------------
 * Client API: device control commands
 *-------------------------------------------------------------*/
#if (FOCUSLAMP_BRIDGE_ENABLE == 1)

/* LED control */
esp_err_t focuslamp_bridge_led_on(int brightness) {
  if (!s_initialized) {
    return ESP_ERR_INVALID_STATE;
  }
  if (brightness < 0) {
    brightness = 0;
  }
  if (brightness > 100) {
    brightness = 100;
  }
  uint8_t raw_brightness = (uint8_t)((brightness * LED_BRIGHTNESS_MAX + 50) / 100);
  /* Restore the current color in steady mode within one service transaction. */
  return led_service_turn_on(raw_brightness);
}

esp_err_t focuslamp_bridge_led_off(void) {
  if (!s_initialized) {
    return ESP_ERR_INVALID_STATE;
  }
  return led_service_turn_off();
}

esp_err_t focuslamp_bridge_led_set_brightness(int brightness) {
  if (!s_initialized) {
    return ESP_ERR_INVALID_STATE;
  }
  if (brightness < 0) {
    brightness = 0;
  }
  if (brightness > 100) {
    brightness = 100;
  }
  uint8_t raw_brightness = (uint8_t)((brightness * LED_BRIGHTNESS_MAX + 50) / 100);
  return led_service_set_brightness(raw_brightness);
}

esp_err_t focuslamp_bridge_led_set_effect(const char *effect) {
  if (!s_initialized) {
    return ESP_ERR_INVALID_STATE;
  }
  if (!effect) return ESP_ERR_INVALID_ARG;
  led_effect_t led_effect;
  if (strcmp(effect, "steady") == 0) {
    led_effect = LED_EFFECT_STEADY;
  } else if (strcmp(effect, "breathing") == 0) {
    led_effect = LED_EFFECT_BREATHING;
  } else if (strcmp(effect, "rainbow") == 0) {
    led_effect = LED_EFFECT_RAINBOW;
  } else if (strcmp(effect, "blinking") == 0) {
    led_effect = LED_EFFECT_BLINKING;
  } else {
    return ESP_ERR_INVALID_ARG;
  }
  return led_service_set_effect(led_effect);
}

esp_err_t focuslamp_bridge_led_set_color(int r, int g, int b) {
  if (!s_initialized) {
    return ESP_ERR_INVALID_STATE;
  }
  r = r < 0 ? 0 : (r > 255 ? 255 : r);
  g = g < 0 ? 0 : (g > 255 ? 255 : g);
  b = b < 0 ? 0 : (b > 255 ? 255 : b);
  return led_service_set_color((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

/* Display control */
esp_err_t focuslamp_bridge_display_on(void) {
  if (!s_initialized) return ESP_ERR_INVALID_STATE;
  return lcd_driver_display_on();
}

esp_err_t focuslamp_bridge_display_off(void) {
  if (!s_initialized) return ESP_ERR_INVALID_STATE;
  return lcd_driver_display_off();
}

esp_err_t focuslamp_bridge_display_set_brightness(int level) {
  if (!s_initialized) return ESP_ERR_INVALID_STATE;
  if (level < 0 || level > 255) return ESP_ERR_INVALID_ARG;
  return lcd_driver_set_backlight((uint8_t)level);
}

/* Reserved local LCD APIs: unsupported until page/expression migration. */
esp_err_t focuslamp_bridge_lcd_next_page(void) {
  return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t focuslamp_bridge_lcd_set_expression(const char *expression) {
  (void)expression;
  return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t focuslamp_bridge_lcd_enable_blink(void) {
  return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t focuslamp_bridge_lcd_disable_blink(void) {
  return ESP_ERR_NOT_SUPPORTED;
}

/* Servo control */
esp_err_t focuslamp_bridge_servo_set_position(int servo_id, int position) {
  if (!s_initialized) return ESP_ERR_INVALID_STATE;
  return motion_servo_set_position_async(servo_id, position);
}

esp_err_t focuslamp_bridge_servo_go_home(int time_ms) {
  if (!s_initialized) return ESP_ERR_INVALID_STATE;
  return motion_home_async(time_ms);
}

/* Focus mode control */
esp_err_t focuslamp_bridge_focus_start(int duration_minutes) {
  if (!s_initialized) return ESP_ERR_INVALID_STATE;
  if (duration_minutes < 1 || duration_minutes > 180) return ESP_ERR_INVALID_ARG;
  return controller_modes_focus_start((uint32_t)duration_minutes);
}

esp_err_t focuslamp_bridge_focus_stop(void) {
  if (!s_initialized) return ESP_ERR_INVALID_STATE;
  return controller_modes_focus_stop();
}

/* Companion mode control */
esp_err_t focuslamp_bridge_companion_start(int duration) {
  if (!s_initialized) return ESP_ERR_INVALID_STATE;
  /* The legacy companion mode ignored duration. It remains untimed here. */
  (void)duration;
  return controller_modes_companion_start();
}

esp_err_t focuslamp_bridge_companion_stop(void) {
  if (!s_initialized) return ESP_ERR_INVALID_STATE;
  return controller_modes_companion_stop();
}

/* Motion control */
esp_err_t focuslamp_bridge_motion_wave(void) {
  if (!s_initialized) return ESP_ERR_INVALID_STATE;
  return motion_action_execute_async("wave", NULL);
}

esp_err_t focuslamp_bridge_motion_nod(void) {
  if (!s_initialized) return ESP_ERR_INVALID_STATE;
  return motion_action_execute_async("nod", NULL);
}

esp_err_t focuslamp_bridge_motion_shake(void) {
  if (!s_initialized) return ESP_ERR_INVALID_STATE;
  return motion_action_execute_async("shake", NULL);
}

esp_err_t focuslamp_bridge_motion_dance(void) {
  if (!s_initialized) return ESP_ERR_INVALID_STATE;
  return motion_action_execute_async("dance", NULL);
}

esp_err_t focuslamp_bridge_motion_greet(void) {
  if (!s_initialized) return ESP_ERR_INVALID_STATE;
  return motion_action_execute_async("greet", NULL);
}

esp_err_t focuslamp_bridge_motion_home(void) {
  if (!s_initialized) return ESP_ERR_INVALID_STATE;
  return motion_action_execute_async("home", NULL);
}

esp_err_t focuslamp_bridge_chat_state(bool active) {
  if (!s_initialized) return ESP_ERR_INVALID_STATE;
  controller_modes_set_voice_active(active);
  return ESP_OK;
}

/*---------------------------------------------------------------
 * MCP tool callbacks
 *-------------------------------------------------------------*/
/* Submission is asynchronous. Do not tell the cloud that acceptance proves motion. */
static esp_mcp_value_t arm_mcp_submission(esp_err_t ret)
{
    if (ret==ESP_OK) {
        return esp_mcp_value_create_string(
            "{\"status\":\"accepted_or_queued\",\"completed\":false,"
            "\"message\":\"动作请求已受理或排队，尚未确认执行成功。请只说已提交，不要说已经动了或完成了。\"}");
    }
    char response[192];
    snprintf(response,sizeof(response),
        "{\"status\":\"rejected\",\"completed\":false,\"error\":\"%s\"}",esp_err_to_name(ret));
    return esp_mcp_value_create_string(response);
}

static esp_mcp_value_t
mcp_tool_led_on(const esp_mcp_property_list_t *properties) {
  int brightness = esp_mcp_property_list_get_property_int(properties, "brightness");
  /* 亮度 0-100 直接透传（0=最暗）。brightness 参数缺失时 SDK 会在参数校验
   * 阶段报错、不会进入本回调，因此不需要"未指定→100"的兜底，0 必须原样下发。 */
  if (brightness < 0) brightness = 0;
  if (brightness > 100) brightness = 100;
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
  /* 亮度 0-100 直接透传（0=最暗）。brightness 参数缺失时 SDK 会在参数校验
   * 阶段报错、不会进入本回调，因此不需要"未指定→100"的兜底，0 必须原样下发。 */
  if (brightness < 0) brightness = 0;
  if (brightness > 100) brightness = 100;
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
mcp_tool_servo_set_position(const esp_mcp_property_list_t *properties) {
  int servo_id = esp_mcp_property_list_get_property_int(properties, "servo_id");
  int position = esp_mcp_property_list_get_property_int(properties, "position");
  ESP_LOGI(TAG, "[MCP] focuslamp.servo.set_position: id=%d pos=%d", servo_id, position);
  return arm_mcp_submission(focuslamp_bridge_servo_set_position(servo_id, position));
}

static esp_mcp_value_t
mcp_tool_servo_go_home(const esp_mcp_property_list_t *properties) {
  int time_ms = esp_mcp_property_list_get_property_int(properties, "time_ms");
  ESP_LOGI(TAG, "[MCP] focuslamp.servo.go_home: time_ms=%d", time_ms);
  return arm_mcp_submission(focuslamp_bridge_servo_go_home(time_ms));
}

static esp_mcp_value_t
mcp_mode_result(esp_err_t ret) {
  controller_modes_status_t status = {0};
  esp_err_t status_ret = controller_modes_get_status(&status);
  if (ret == ESP_OK && status_ret != ESP_OK) ret = status_ret;
  char response[384];
  snprintf(response, sizeof(response),
      "{\"ok\":%s,\"effects_async\":true,\"mode\":\"%s\","
      "\"remaining_sec\":%lu,\"paused\":%s,\"error\":\"%s\","
      "\"message\":\"模式状态已返回；陪伴联动由后台异步执行，完成状态请以设备实际响应为准。\"}",
      ret == ESP_OK ? "true" : "false", controller_modes_name(status.mode),
      (unsigned long)status.remaining_sec, status.paused ? "true" : "false",
      ret == ESP_OK ? "" : esp_err_to_name(ret));
  return esp_mcp_value_create_string(response);
}

static esp_mcp_value_t
mcp_tool_focus_start(const esp_mcp_property_list_t *properties) {
  int duration = esp_mcp_property_list_get_property_int(properties, "duration");
  if (duration <= 0) duration = 30;
  ESP_LOGI(TAG, "[MCP] focuslamp.focus.start: duration=%d", duration);

  return mcp_mode_result(focuslamp_bridge_focus_start(duration));
}

static esp_mcp_value_t
mcp_tool_focus_stop(const esp_mcp_property_list_t *properties) {
  (void)properties;
  ESP_LOGI(TAG, "[MCP] focuslamp.focus.stop");

  return mcp_mode_result(focuslamp_bridge_focus_stop());
}

static esp_mcp_value_t
mcp_tool_companion_start(const esp_mcp_property_list_t *properties) {
  (void)properties;
  ESP_LOGI(TAG, "[MCP] focuslamp.companion.start");

  esp_err_t ret = focuslamp_bridge_companion_start(0);
  if (ret != ESP_OK) return mcp_mode_result(ret);
  return esp_mcp_value_create_string(
      "{\"ok\":true,\"mode\":\"companion\",\"state_only\":false,\"effects_async\":true,"
      "\"announcement\":\"主人，跟Focus聊聊天吧～\","
      "\"message\":\"请播报announcement。陪伴状态已设置，灯光与灯头联动正在异步执行；"
      "不要宣称摄像头、手势、灯光已全部成功。\"}");
}

static esp_mcp_value_t
mcp_tool_companion_stop(const esp_mcp_property_list_t *properties) {
  (void)properties;
  ESP_LOGI(TAG, "[MCP] focuslamp.companion.stop");

  return mcp_mode_result(focuslamp_bridge_companion_stop());
}

static esp_mcp_value_t
mcp_tool_motion_wave(const esp_mcp_property_list_t *properties) {
  (void)properties;
  ESP_LOGI(TAG, "[MCP] focuslamp.arm.wave");
  return arm_mcp_submission(focuslamp_bridge_motion_wave());
}

static esp_mcp_value_t
mcp_tool_motion_nod(const esp_mcp_property_list_t *properties) {
  (void)properties;
  ESP_LOGI(TAG, "[MCP] focuslamp.arm.nod");
  return arm_mcp_submission(focuslamp_bridge_motion_nod());
}

static esp_mcp_value_t
mcp_tool_motion_shake(const esp_mcp_property_list_t *properties) {
  (void)properties;
  ESP_LOGI(TAG, "[MCP] focuslamp.arm.shake");
  return arm_mcp_submission(focuslamp_bridge_motion_shake());
}

static esp_mcp_value_t
mcp_tool_motion_dance(const esp_mcp_property_list_t *properties) {
  (void)properties;
  ESP_LOGI(TAG, "[MCP] focuslamp.arm.dance");
  return arm_mcp_submission(focuslamp_bridge_motion_dance());
}

static esp_mcp_value_t
mcp_tool_motion_greet(const esp_mcp_property_list_t *properties) {
  (void)properties;
  ESP_LOGI(TAG, "[MCP] focuslamp.arm.greet");
  return arm_mcp_submission(focuslamp_bridge_motion_greet());
}

static esp_mcp_value_t
mcp_tool_motion_home(const esp_mcp_property_list_t *properties) {
  (void)properties;
  ESP_LOGI(TAG, "[MCP] focuslamp.arm.home");
  return arm_mcp_submission(focuslamp_bridge_motion_home());
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

  t = esp_mcp_tool_create("self.focuslamp.display.set_brightness", "设置底座屏幕数字背光：0关闭，1-255开启；不支持连续调光", mcp_tool_display_set_brightness);
  if (t) { esp_mcp_property_t *p = esp_mcp_property_create_with_range("level", 0, 255); esp_mcp_tool_add_property(t, p); esp_mcp_add_tool(mcp, t); }

  /* LCD page/expression/blink are not migrated; do not advertise tools. */

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
  t = esp_mcp_tool_create("self.focuslamp.focus.start", "开启本地专注计时状态(duration分钟1-180,默认30；对话时暂停；暂不联动灯光、屏幕、白噪声或机械臂)", mcp_tool_focus_start);
  if (t) { esp_mcp_property_t *p = esp_mcp_property_create_with_range("duration", 1, 180); esp_mcp_tool_add_property(t, p); esp_mcp_add_tool(mcp, t); }

  t = esp_mcp_tool_create("self.focuslamp.focus.stop", "停止本地专注计时状态(只停止专注，不改变陪伴状态或外设)", mcp_tool_focus_stop);
  if (t) esp_mcp_add_tool(mcp, t);

  /* Companion mode tools (2) */
  t = esp_mcp_tool_create("self.focuslamp.companion.start", "开启陪伴模式：底座呼吸灯、关闭头灯、开心表情、启动摄像头及手势处理。联动异步执行；播报返回的announcement，不宣称动作已完成", mcp_tool_companion_start);
  if (t) esp_mcp_add_tool(mcp, t);

  t = esp_mcp_tool_create("self.focuslamp.companion.stop", "关闭陪伴模式：停止自动呼吸灯，恢复灯头正常结果处理，不归位机械臂", mcp_tool_companion_stop);
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

  ESP_LOGI(TAG, "FocusLamp local MCP tools registered (20 tools; LCD page/expression/blink not advertised)");
  return ESP_OK;
}

/*---------------------------------------------------------------
 * Init / Deinit
 *-------------------------------------------------------------*/

esp_err_t focuslamp_bridge_init(void) {
  ESP_RETURN_ON_FALSE(!s_initialized, ESP_ERR_INVALID_STATE, TAG,
                      "Already initialized");

  s_initialized = true;
  ESP_LOGI(TAG, "FocusLamp local bridge initialized (no remote base HTTP)");
  return ESP_OK;
}

void focuslamp_bridge_deinit(void) {
  if (!s_initialized) {
    return;
  }

  s_initialized = false;
  ESP_LOGI(TAG, "FocusLamp bridge deinitialized");
}

#else /* FOCUSLAMP_BRIDGE_ENABLE == 0 */

/* Stub implementations when component is disabled */

esp_err_t focuslamp_bridge_init(void) { return ESP_ERR_NOT_SUPPORTED; }
void focuslamp_bridge_deinit(void) {}
esp_err_t focuslamp_bridge_register_mcp_tools(esp_mcp_t *mcp) {
  (void)mcp;
  return ESP_ERR_NOT_SUPPORTED;
}

#endif /* FOCUSLAMP_BRIDGE_ENABLE */
