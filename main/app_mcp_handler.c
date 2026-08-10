/*
 * app_mcp_handler.c - Remote MCP (JSON-RPC 2.0) tool handler for FocusLamp
 *
 * Implements MCP protocol for remote control of LED, LCD, Display, and System.
 * Conforms to MCP specification (protocol version 2025-11-25).
 *
 * Data flow:
 *   Remote client ──WebSocket──> ws_manager (/mcp endpoint)
 *     └─> ws_data_handler ──> app_mcp_handler_handle_data()
 *       └─> JSON-RPC 2.0 parse ──> tool dispatch ──> hardware API call
 *       └─> response sent back via ws_manager_server_send_text()
 *
 * Transport: WebSocket /mcp endpoint (replaces the former TCP wifi_comm_module
 * link). Initialized by network_manager_init() which calls app_mcp_handler_init()
 * after ws_manager_server_start().
 */

#include "app_mcp_handler.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "cJSON.h"

#include "websocket_manager.h"

/* Hardware modules */
#include "lcd_module.h"
#include "lcd_driver.h"
#include "led_service.h"
#include "device_state.h"
#include "servo_service.h"

static const char *TAG = "mcp_handler";

#define MCP_PROTOCOL_VERSION "2025-11-25"
#define MCP_RESPONSE_BUF_SIZE 2048

/* ===================== Tool Metadata ===================== */

typedef struct {
    const char *name;
    const char *description;
    const char *input_schema; /* JSON schema string */
} tool_meta_t;

static const tool_meta_t s_tool_meta[] = {
    /* --- LED tools --- */
    {"led.on", "Turn on the LED light with optional brightness (0-255)",
     "{\"type\":\"object\",\"properties\":{\"brightness\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255}},"
     "\"additionalProperties\":false}"},
    {"led.off", "Turn off the LED light",
     "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"},
    {"led.set_brightness", "Set LED brightness (0-255)",
     "{\"type\":\"object\",\"properties\":{\"brightness\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255}},"
     "\"required\":[\"brightness\"]}"},
    {"led.set_effect", "Set LED lighting effect: steady/breathing/rainbow/blinking",
     "{\"type\":\"object\",\"properties\":{\"effect\":{\"type\":\"string\",\"enum\":[\"steady\",\"breathing\","
     "\"rainbow\",\"blinking\"]}},\"required\":[\"effect\"]}"},
    {"led.set_color", "Set LED RGB color",
     "{\"type\":\"object\",\"properties\":{\"r\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},"
     "\"g\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},\"b\":{\"type\":\"integer\",\"minimum\":0,"
     "\"maximum\":255}},\"required\":[\"r\",\"g\",\"b\"]}"},
    {"led.get_status", "Get LED current status (brightness, effect, mode)",
     "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"},

    /* --- Display/LCD tools --- */
    {"display.on", "Turn on the LCD display",
     "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"},
    {"display.off", "Turn off the LCD display",
     "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"},
    {"display.set_brightness", "Set display backlight brightness (0-255)",
     "{\"type\":\"object\",\"properties\":{\"level\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255}},"
     "\"required\":[\"level\"]}"},
    {"lcd.switch_page", "Switch LCD to the next display page",
     "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"},
    {"lcd.set_expression", "Set LCD expression: normal/happy/sad/angry/surprised/sleepy",
     "{\"type\":\"object\",\"properties\":{\"expression\":{\"type\":\"string\",\"enum\":[\"normal\",\"happy\",\"sad\","
     "\"angry\",\"surprised\",\"sleepy\"]}},\"required\":[\"expression\"]}"},
    {"lcd.enable_blink", "Enable automatic eye blinking on LCD",
     "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"},
    {"lcd.disable_blink", "Disable automatic eye blinking on LCD",
     "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"},

    /* --- System tools --- */
    {"system.get_info", "Get device information (chip, firmware, uptime, state)",
     "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"},

    /* --- Radar tools --- */
    {"radar.get_status", "Get radar status (presence, heart rate, breath rate, distance, HRV)",
     "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"},

    /* --- Servo tools (5 servos: 1 EM3 + 4 LX) ---
     * servo_id mapping: 0=EM3 (range 0-3000), 1-4=LX (range 0-1000, bus IDs 1,2,3,5) */
    {"servo.set_position", "Set single servo position (servo_id 0=EM3 0-3000, 1-4=LX 0-1000)",
     "{\"type\":\"object\",\"properties\":{\"servo_id\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":4},"
     "\"position\":{\"type\":\"integer\",\"minimum\":0}},\"required\":[\"servo_id\",\"position\"]}"},
    {"servo.get_position", "Get current position of a servo (servo_id 0-4)",
     "{\"type\":\"object\",\"properties\":{\"servo_id\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":4}},"
     "\"required\":[\"servo_id\"]}"},
    {"servo.go_home", "Move all servos to home position",
     "{\"type\":\"object\",\"properties\":{\"time_ms\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":65535}},"
     "\"additionalProperties\":false}"},
};

#define TOOL_COUNT (sizeof(s_tool_meta) / sizeof(s_tool_meta[0]))

/* ===================== JSON-RPC Helpers ===================== */

static void build_error_response(char *buf, int buf_size, int id, int code, const char *message)
{
    /* id can be 0 or the request's id */
    snprintf(buf, buf_size,
             "{\"jsonrpc\":\"2.0\",\"id\":%d,\"error\":{\"code\":%d,\"message\":\"%s\"}}",
             id, code, message ? message : "");
}

static void build_success_response(char *buf, int buf_size, int id, const char *result_json)
{
    snprintf(buf, buf_size,
             "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":%s}",
             id, result_json ? result_json : "null");
}

/* Send a JSON-RPC response back to the WebSocket client that issued the request.
 * Replaces the former wifi_comm_module_send() path now that the transport is
 * WebSocket (/mcp endpoint) instead of the legacy TCP link. */
static void mcp_send_response(int client_fd, const char *buf)
{
    if (client_fd >= 0 && buf != NULL && buf[0] != '\0') {
        ws_manager_server_send_text(client_fd, buf, (int)strlen(buf));
    }
}

/* ===================== Tool Callbacks ===================== */

/* --- LED Callbacks --- */

static esp_err_t cb_led_on(const cJSON *args, char *resp, int resp_size)
{
    int brightness = 255;
    if (args) {
        const cJSON *b = cJSON_GetObjectItem(args, "brightness");
        if (cJSON_IsNumber(b)) brightness = b->valueint;
    }
    if (brightness < 0) brightness = 0;
    if (brightness > 255) brightness = 255;

    /* Use existing service API */
    led_service_set_brightness((uint8_t)brightness);
    /* Default to steady white light */
    led_service_set_mode(LED_MODE_WHITE);
    led_service_set_effect(LED_EFFECT_STEADY);

    ESP_LOGI(TAG, "MCP: led.on brightness=%d", brightness);
    snprintf(resp, resp_size, "{\"ok\":true,\"brightness\":%d}", brightness);
    return ESP_OK;
}

static esp_err_t cb_led_off(const cJSON *args, char *resp, int resp_size)
{
    (void)args;
    led_service_turn_off();
    ESP_LOGI(TAG, "MCP: led.off");
    snprintf(resp, resp_size, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t cb_led_set_brightness(const cJSON *args, char *resp, int resp_size)
{
    if (!args) {
        snprintf(resp, resp_size, "{\"ok\":false,\"error\":\"missing brightness\"}");
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *b = cJSON_GetObjectItem(args, "brightness");
    if (!cJSON_IsNumber(b)) {
        snprintf(resp, resp_size, "{\"ok\":false,\"error\":\"invalid brightness\"}");
        return ESP_ERR_INVALID_ARG;
    }
    int brightness = b->valueint;
    if (brightness < 0) brightness = 0;
    if (brightness > 255) brightness = 255;

    led_service_set_brightness((uint8_t)brightness);
    ESP_LOGI(TAG, "MCP: led.set_brightness %d", brightness);
    snprintf(resp, resp_size, "{\"ok\":true,\"brightness\":%d}", brightness);
    return ESP_OK;
}

static esp_err_t cb_led_set_effect(const cJSON *args, char *resp, int resp_size)
{
    if (!args) {
        snprintf(resp, resp_size, "{\"ok\":false,\"error\":\"missing effect\"}");
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *e = cJSON_GetObjectItem(args, "effect");
    if (!cJSON_IsString(e) || !e->valuestring) {
        snprintf(resp, resp_size, "{\"ok\":false,\"error\":\"invalid effect\"}");
        return ESP_ERR_INVALID_ARG;
    }

    led_effect_t effect;
    if (strcmp(e->valuestring, "steady") == 0) effect = LED_EFFECT_STEADY;
    else if (strcmp(e->valuestring, "breathing") == 0) effect = LED_EFFECT_BREATHING;
    else if (strcmp(e->valuestring, "rainbow") == 0) effect = LED_EFFECT_RAINBOW;
    else if (strcmp(e->valuestring, "blinking") == 0) effect = LED_EFFECT_BLINKING;
    else {
        snprintf(resp, resp_size, "{\"ok\":false,\"error\":\"unknown effect: %s\"}", e->valuestring);
        return ESP_ERR_INVALID_ARG;
    }

    led_service_set_effect(effect);
    ESP_LOGI(TAG, "MCP: led.set_effect %s", e->valuestring);
    snprintf(resp, resp_size, "{\"ok\":true,\"effect\":\"%s\"}", e->valuestring);
    return ESP_OK;
}

static esp_err_t cb_led_set_color(const cJSON *args, char *resp, int resp_size)
{
    if (!args) {
        snprintf(resp, resp_size, "{\"ok\":false,\"error\":\"missing color\"}");
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *r = cJSON_GetObjectItem(args, "r");
    const cJSON *g = cJSON_GetObjectItem(args, "g");
    const cJSON *b = cJSON_GetObjectItem(args, "b");
    if (!cJSON_IsNumber(r) || !cJSON_IsNumber(g) || !cJSON_IsNumber(b)) {
        snprintf(resp, resp_size, "{\"ok\":false,\"error\":\"invalid r/g/b values\"}");
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t red   = (uint8_t)(r->valueint & 0xFF);
    uint8_t green = (uint8_t)(g->valueint & 0xFF);
    uint8_t blue  = (uint8_t)(b->valueint & 0xFF);

    led_service_set_mode(LED_MODE_COLOR);
    led_service_set_color(red, green, blue);
    ESP_LOGI(TAG, "MCP: led.set_color (%d,%d,%d)", red, green, blue);
    snprintf(resp, resp_size, "{\"ok\":true,\"color\":{\"r\":%d,\"g\":%d,\"b\":%d}}", red, green, blue);
    return ESP_OK;
}

static esp_err_t cb_led_get_status(const cJSON *args, char *resp, int resp_size)
{
    (void)args;
    led_module_snapshot_t snap;
    esp_err_t ret = led_service_get_snapshot(&snap);
    if (ret != ESP_OK) {
        snprintf(resp, resp_size, "{\"ok\":false,\"error\":\"snapshot failed\"}");
        return ret;
    }
    bool is_on = (snap.mode != LED_MODULE_MODE_OFF) && snap.initialized;
    snprintf(resp, resp_size,
             "{\"ok\":true,\"brightness\":%d,\"mode\":%d,\"color\":{\"r\":%d,\"g\":%d,\"b\":%d},"
             "\"is_on\":%s,\"brightness_level\":%d}",
             snap.brightness, (int)snap.mode,
             snap.color.red, snap.color.green, snap.color.blue,
             is_on ? "true" : "false",
             snap.brightness_level);
    return ESP_OK;
}

/* --- Display Callbacks --- */

static esp_err_t cb_display_on(const cJSON *args, char *resp, int resp_size)
{
    (void)args;
    esp_err_t ret = lcd_driver_display_on();
    ESP_LOGI(TAG, "MCP: display.on -> %s", esp_err_to_name(ret));
    snprintf(resp, resp_size, "{\"ok\":%s}", ret == ESP_OK ? "true" : "false");
    return ret;
}

static esp_err_t cb_display_off(const cJSON *args, char *resp, int resp_size)
{
    (void)args;
    esp_err_t ret = lcd_driver_display_off();
    ESP_LOGI(TAG, "MCP: display.off -> %s", esp_err_to_name(ret));
    snprintf(resp, resp_size, "{\"ok\":%s}", ret == ESP_OK ? "true" : "false");
    return ret;
}

static esp_err_t cb_display_set_brightness(const cJSON *args, char *resp, int resp_size)
{
    if (!args) {
        snprintf(resp, resp_size, "{\"ok\":false,\"error\":\"missing level\"}");
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *l = cJSON_GetObjectItem(args, "level");
    if (!cJSON_IsNumber(l)) {
        snprintf(resp, resp_size, "{\"ok\":false,\"error\":\"invalid level\"}");
        return ESP_ERR_INVALID_ARG;
    }
    int level = l->valueint;
    if (level < 0) level = 0;
    if (level > 255) level = 255;

    lcd_driver_set_backlight((uint8_t)level);
    ESP_LOGI(TAG, "MCP: display.set_brightness %d", level);
    snprintf(resp, resp_size, "{\"ok\":true,\"level\":%d}", level);
    return ESP_OK;
}

/* --- LCD Callbacks --- */

static esp_err_t cb_lcd_switch_page(const cJSON *args, char *resp, int resp_size)
{
    (void)args;
    esp_err_t ret = lcd_module_next_page();
    ESP_LOGI(TAG, "MCP: lcd.switch_page -> %s", esp_err_to_name(ret));
    snprintf(resp, resp_size, "{\"ok\":%s}", ret == ESP_OK ? "true" : "false");
    return ret;
}

static const char *s_expr_names[] = {
    "normal", "happy", "sad", "angry", "surprised", "sleepy"
};

static esp_err_t cb_lcd_set_expression(const cJSON *args, char *resp, int resp_size)
{
    if (!args) {
        snprintf(resp, resp_size, "{\"ok\":false,\"error\":\"missing expression\"}");
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *ex = cJSON_GetObjectItem(args, "expression");
    if (!cJSON_IsString(ex) || !ex->valuestring) {
        snprintf(resp, resp_size, "{\"ok\":false,\"error\":\"invalid expression\"}");
        return ESP_ERR_INVALID_ARG;
    }

    lcd_expression_t expr = LCD_EXPRESSION_NORMAL;
    bool found = false;
    for (size_t i = 0; i < sizeof(s_expr_names) / sizeof(s_expr_names[0]); i++) {
        if (strcmp(ex->valuestring, s_expr_names[i]) == 0) {
            expr = (lcd_expression_t)i;
            found = true;
            break;
        }
    }
    if (!found) {
        snprintf(resp, resp_size, "{\"ok\":false,\"error\":\"unknown expression: %s\"}", ex->valuestring);
        return ESP_ERR_INVALID_ARG;
    }

    lcd_module_set_expression(expr);
    ESP_LOGI(TAG, "MCP: lcd.set_expression %s", ex->valuestring);
    snprintf(resp, resp_size, "{\"ok\":true,\"expression\":\"%s\"}", ex->valuestring);
    return ESP_OK;
}

static esp_err_t cb_lcd_enable_blink(const cJSON *args, char *resp, int resp_size)
{
    (void)args;
    lcd_module_set_auto_blink(true);
    ESP_LOGI(TAG, "MCP: lcd.enable_blink");
    snprintf(resp, resp_size, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t cb_lcd_disable_blink(const cJSON *args, char *resp, int resp_size)
{
    (void)args;
    lcd_module_set_auto_blink(false);
    ESP_LOGI(TAG, "MCP: lcd.disable_blink");
    snprintf(resp, resp_size, "{\"ok\":true}");
    return ESP_OK;
}

/* --- System Callbacks --- */

static esp_err_t cb_system_get_info(const cJSON *args, char *resp, int resp_size)
{
    (void)args;

    /* Gather system info */
    const char *chip = CONFIG_IDF_TARGET;
    uint32_t free_heap = esp_get_free_heap_size();
    int64_t uptime_ms = esp_timer_get_time() / 1000;

    snprintf(resp, resp_size,
             "{\"ok\":true,\"chip\":\"%s\",\"free_heap\":%lu,\"uptime_ms\":%lld}",
             chip, (unsigned long)free_heap, (long long)uptime_ms);
    return ESP_OK;
}

/* --- Radar Callbacks --- */

static esp_err_t cb_radar_get_status(const cJSON *args, char *resp, int resp_size)
{
    (void)args;
    device_state_t state = {0};
    esp_err_t ret = device_state_get(&state);
    if (ret != ESP_OK) {
        snprintf(resp, resp_size, "{\"ok\":false,\"error\":\"state get failed\"}");
        return ret;
    }
    snprintf(resp, resp_size,
             "{\"ok\":true,\"present\":%s,\"heart_rate_bpm\":%.1f,\"breath_rate_bpm\":%.1f,"
             "\"distance_cm\":%.1f,\"hrv_sdnn\":%.1f,\"hrv_rmssd\":%.1f,"
             "\"heart_rate_valid\":%s,\"distance_valid\":%s}",
             state.radar.present ? "true" : "false",
             state.radar.heart_rate_bpm,
             state.radar.breath_rate_bpm,
             state.radar.distance_cm,
             state.radar.hrv_sdnn,
             state.radar.hrv_rmssd,
             state.radar.heart_rate_valid ? "true" : "false",
             state.radar.distance_valid ? "true" : "false");
    return ESP_OK;
}

/* --- Servo Callbacks --- */
/* Servo ID mapping: 0=EM3 (bus ID 4, range 0-3000), 1-4=LX (bus IDs 1,2,3,5, range 0-1000).
 * Because servo_service_set_all_positions moves all 5 servos as a group, single-servo
 * set is implemented by reading current positions from device_state, modifying only
 * the target servo, and calling set_all_positions (non-target servos stay put). */

#define MCP_SERVO_DEFAULT_TIME_MS 1000

static esp_err_t cb_servo_set_position(const cJSON *args, char *resp, int resp_size)
{
    if (!args) {
        snprintf(resp, resp_size, "{\"ok\":false,\"error\":\"missing args\"}");
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *id_item = cJSON_GetObjectItem(args, "servo_id");
    const cJSON *pos_item = cJSON_GetObjectItem(args, "position");
    if (!cJSON_IsNumber(id_item) || !cJSON_IsNumber(pos_item)) {
        snprintf(resp, resp_size, "{\"ok\":false,\"error\":\"invalid servo_id or position\"}");
        return ESP_ERR_INVALID_ARG;
    }

    int servo_id = id_item->valueint;
    int position = pos_item->valueint;
    if (servo_id < 0 || servo_id > 4) {
        snprintf(resp, resp_size, "{\"ok\":false,\"error\":\"servo_id out of range (0-4)\"}");
        return ESP_ERR_INVALID_ARG;
    }
    /* Clamp to valid range per servo type */
    if (servo_id == 0) {
        if (position < 0) position = 0;
        if (position > 3000) position = 3000;
    } else {
        if (position < 0) position = 0;
        if (position > 1000) position = 1000;
    }

    /* Build full positions struct from current device state, modify target only */
    device_state_t dev_state = {0};
    device_state_get(&dev_state);

    servo_service_positions_t target = {
        .em3_pos = dev_state.servo.em3_pos,
        .lx_pos  = { dev_state.servo.lx_pos[0], dev_state.servo.lx_pos[1],
                     dev_state.servo.lx_pos[2], dev_state.servo.lx_pos[3] },
    };

    if (servo_id == 0) {
        target.em3_pos = (int16_t)position;
    } else {
        target.lx_pos[servo_id - 1] = (int16_t)position;
    }

    esp_err_t ret = servo_service_set_all_positions(&target, MCP_SERVO_DEFAULT_TIME_MS);
    if (ret == ESP_OK) {
        /* servo_service_set_all_positions does not sync device_state internally
         * (unlike the single-servo set_position), so update it here to keep
         * servo.get_position consistent immediately after a set. */
        device_state_set_servo(target.em3_pos, target.lx_pos, true);
    }
    ESP_LOGI(TAG, "MCP: servo.set_position id=%d pos=%d -> %s", servo_id, position, esp_err_to_name(ret));
    snprintf(resp, resp_size, "{\"ok\":%s,\"servo_id\":%d,\"position\":%d}",
             ret == ESP_OK ? "true" : "false", servo_id, position);
    return ret;
}

static esp_err_t cb_servo_get_position(const cJSON *args, char *resp, int resp_size)
{
    if (!args) {
        snprintf(resp, resp_size, "{\"ok\":false,\"error\":\"missing args\"}");
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *id_item = cJSON_GetObjectItem(args, "servo_id");
    if (!cJSON_IsNumber(id_item)) {
        snprintf(resp, resp_size, "{\"ok\":false,\"error\":\"invalid servo_id\"}");
        return ESP_ERR_INVALID_ARG;
    }

    int servo_id = id_item->valueint;
    if (servo_id < 0 || servo_id > 4) {
        snprintf(resp, resp_size, "{\"ok\":false,\"error\":\"servo_id out of range (0-4)\"}");
        return ESP_ERR_INVALID_ARG;
    }

    device_state_t dev_state = {0};
    device_state_get(&dev_state);

    int position;
    if (servo_id == 0) {
        position = dev_state.servo.em3_pos;
    } else {
        position = dev_state.servo.lx_pos[servo_id - 1];
    }

    ESP_LOGD(TAG, "MCP: servo.get_position id=%d pos=%d", servo_id, position);
    snprintf(resp, resp_size, "{\"ok\":true,\"servo_id\":%d,\"position\":%d}", servo_id, position);
    return ESP_OK;
}

static esp_err_t cb_servo_go_home(const cJSON *args, char *resp, int resp_size)
{
    int time_ms = MCP_SERVO_DEFAULT_TIME_MS;
    if (args) {
        const cJSON *t = cJSON_GetObjectItem(args, "time_ms");
        if (cJSON_IsNumber(t)) {
            time_ms = t->valueint;
            if (time_ms < 0) time_ms = 0;
            if (time_ms > 65535) time_ms = 65535;
        }
    }
    esp_err_t ret = servo_service_go_home((uint16_t)time_ms);
    if (ret == ESP_OK) {
        /* Sync device_state with home positions (go_home uses set_all_positions
         * internally which does not sync device_state). */
        servo_service_positions_t home = {0};
        if (servo_service_get_home_positions(&home) == ESP_OK) {
            device_state_set_servo(home.em3_pos, home.lx_pos, true);
        }
    }
    ESP_LOGI(TAG, "MCP: servo.go_home time=%dms -> %s", time_ms, esp_err_to_name(ret));
    snprintf(resp, resp_size, "{\"ok\":%s,\"time_ms\":%d}", ret == ESP_OK ? "true" : "false", time_ms);
    return ret;
}

/* ===================== Tool Dispatch Table ===================== */

typedef esp_err_t (*tool_cb_t)(const cJSON *args, char *resp, int resp_size);

typedef struct {
    const char *name;
    tool_cb_t callback;
} tool_entry_t;

static const tool_entry_t s_tool_dispatch[] = {
    {"led.on",                 cb_led_on},
    {"led.off",                cb_led_off},
    {"led.set_brightness",     cb_led_set_brightness},
    {"led.set_effect",         cb_led_set_effect},
    {"led.set_color",          cb_led_set_color},
    {"led.get_status",         cb_led_get_status},
    {"display.on",             cb_display_on},
    {"display.off",            cb_display_off},
    {"display.set_brightness", cb_display_set_brightness},
    {"lcd.switch_page",        cb_lcd_switch_page},
    {"lcd.set_expression",    cb_lcd_set_expression},
    {"lcd.enable_blink",      cb_lcd_enable_blink},
    {"lcd.disable_blink",     cb_lcd_disable_blink},
    {"system.get_info",        cb_system_get_info},
    {"radar.get_status",       cb_radar_get_status},
    {"servo.set_position",     cb_servo_set_position},
    {"servo.get_position",     cb_servo_get_position},
    {"servo.go_home",          cb_servo_go_home},
};

#define DISPATCH_COUNT (sizeof(s_tool_dispatch) / sizeof(s_tool_dispatch[0]))

/* ===================== JSON-RPC Method Handlers ===================== */

static void handle_initialize(char *buf, int buf_size, int id)
{
    char result[384];
    snprintf(result, sizeof(result),
             "{\"protocolVersion\":\"%s\",\"capabilities\":{\"tools\":{}},"
             "\"serverInfo\":{\"name\":\"focuslamp-mcp\",\"version\":\"1.0.0\"}}",
             MCP_PROTOCOL_VERSION);
    build_success_response(buf, buf_size, id, result);
}

static void handle_tools_list(char *buf, int buf_size, int id)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *tools_array = cJSON_CreateArray();

    for (int i = 0; i < (int)TOOL_COUNT; i++) {
        cJSON *tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", s_tool_meta[i].name);
        cJSON_AddStringToObject(tool, "description", s_tool_meta[i].description);

        cJSON *schema = cJSON_Parse(s_tool_meta[i].input_schema);
        if (schema) {
            cJSON_AddItemToObject(tool, "inputSchema", schema);
        }
        cJSON_AddItemToArray(tools_array, tool);
    }

    cJSON_AddItemToObject(root, "tools", tools_array);
    char *result_str = cJSON_PrintUnformatted(root);
    if (result_str) {
        build_success_response(buf, buf_size, id, result_str);
        free(result_str);
    } else {
        build_error_response(buf, buf_size, id, -32603, "Failed to build tools list");
    }
    cJSON_Delete(root);
}

static void handle_tools_call(cJSON *request, char *buf, int buf_size, int id)
{
    cJSON *params = cJSON_GetObjectItem(request, "params");
    if (!params) {
        build_error_response(buf, buf_size, id, -32602, "Missing params");
        return;
    }

    cJSON *name_item = cJSON_GetObjectItem(params, "name");
    if (!cJSON_IsString(name_item) || !name_item->valuestring) {
        build_error_response(buf, buf_size, id, -32602, "Missing or invalid tool name");
        return;
    }

    const char *tool_name = name_item->valuestring;
    cJSON *arguments = cJSON_GetObjectItem(params, "arguments");

    /* Find and execute tool */
    for (int i = 0; i < (int)DISPATCH_COUNT; i++) {
        if (strcmp(s_tool_dispatch[i].name, tool_name) == 0) {
            char tool_resp[512] = {0};
            esp_err_t ret = s_tool_dispatch[i].callback(arguments, tool_resp, sizeof(tool_resp));

            /* Build MCP-style response with content array */
            cJSON *result = cJSON_CreateObject();
            cJSON *content_array = cJSON_CreateArray();
            cJSON *content_item = cJSON_CreateObject();
            cJSON_AddStringToObject(content_item, "type", "text");
            cJSON_AddStringToObject(content_item, "text",
                                    (ret == ESP_OK && tool_resp[0] != '\0') ? tool_resp : "{}");
            cJSON_AddItemToArray(content_array, content_item);
            cJSON_AddItemToObject(result, "content", content_array);
            cJSON_AddBoolToObject(result, "isError", (ret != ESP_OK));

            char *result_str = cJSON_PrintUnformatted(result);
            if (result_str) {
                build_success_response(buf, buf_size, id, result_str);
                free(result_str);
            } else {
                build_error_response(buf, buf_size, id, -32603, "Failed to build response");
            }
            cJSON_Delete(result);
            return;
        }
    }

    build_error_response(buf, buf_size, id, -32601, "Unknown tool");
}

/* ===================== Main Handler ===================== */

esp_err_t app_mcp_handler_handle_data(const char *data, int data_len, int client_fd)
{
    if (!data || data_len == 0) return ESP_ERR_INVALID_ARG;

    /* Make a null-terminated copy of the request */
    char request_buf[MCP_RESPONSE_BUF_SIZE];
    int copy_len = data_len;
    if (copy_len >= (int)sizeof(request_buf)) {
        copy_len = sizeof(request_buf) - 1;
    }
    memcpy(request_buf, data, copy_len);
    request_buf[copy_len] = '\0';

    /* Parse JSON */
    cJSON *request = cJSON_Parse(request_buf);
    if (!request) {
        ESP_LOGW(TAG, "Invalid JSON-RPC request");
        char err_buf[256];
        build_error_response(err_buf, sizeof(err_buf), 0, -32700, "Parse error");
        mcp_send_response(client_fd, err_buf);
        return ESP_ERR_INVALID_ARG;
    }

    /* Extract request id */
    int id = 0;
    cJSON *id_item = cJSON_GetObjectItem(request, "id");
    if (cJSON_IsNumber(id_item)) {
        id = id_item->valueint;
    }

    /* Extract method */
    cJSON *method_item = cJSON_GetObjectItem(request, "method");
    if (!cJSON_IsString(method_item) || !method_item->valuestring) {
        char err_buf[256];
        build_error_response(err_buf, sizeof(err_buf), id, -32600, "Missing method");
        mcp_send_response(client_fd, err_buf);
        cJSON_Delete(request);
        return ESP_ERR_INVALID_ARG;
    }

    const char *method = method_item->valuestring;
    ESP_LOGD(TAG, "MCP request: method=%s, id=%d", method, id);

    /* Dispatch method */
    char response[MCP_RESPONSE_BUF_SIZE] = {0};

    if (strcmp(method, "initialize") == 0) {
        handle_initialize(response, sizeof(response), id);
    } else if (strcmp(method, "tools.list") == 0) {
        handle_tools_list(response, sizeof(response), id);
    } else if (strcmp(method, "tools.call") == 0) {
        handle_tools_call(request, response, sizeof(response), id);
    } else if (strcmp(method, "ping") == 0) {
        build_success_response(response, sizeof(response), id, "{}");
    } else {
        build_error_response(response, sizeof(response), id, -32601, "Method not found");
        cJSON_Delete(request);
        mcp_send_response(client_fd, response);
        return ESP_ERR_NOT_SUPPORTED;
    }

    cJSON_Delete(request);

    /* Send response back via WebSocket (/mcp endpoint) */
    mcp_send_response(client_fd, response);

    return ESP_OK;
}

/* ===================== WebSocket DATA Handler ===================== */

/* Receives all WebSocket DATA events from ws_manager. Only /mcp text frames
 * are routed to the MCP JSON-RPC engine; other URIs (e.g. /ws echo) are
 * ignored here so they can be handled by their own subscribers. */
static void ws_data_handler(ws_manager_event_t event, void *data)
{
    if (event != WS_MANAGER_EVENT_DATA || data == NULL) {
        return;
    }
    const ws_manager_data_t *msg = (const ws_manager_data_t *)data;
    if (msg->type != WS_DATA_TYPE_TEXT) {
        return;
    }
    if (msg->uri == NULL || strcmp(msg->uri, "/mcp") != 0) {
        return;
    }
    app_mcp_handler_handle_data(msg->data, msg->data_len, msg->client_fd);
}

esp_err_t app_mcp_handler_init(void)
{
    /* Register WebSocket DATA handler so /mcp frames are dispatched here.
     * Requires ws_manager_init() + ws_manager_server_start() to have run. */
    esp_err_t ret = ws_manager_register_handler(WS_MANAGER_EVENT_DATA, ws_data_handler);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register WS DATA handler: %s", esp_err_to_name(ret));
        /* Non-fatal: MCP over WS won't work, but system continues */
    }

    ESP_LOGI(TAG, "MCP handler initialized (protocol=%s, tools=%d, transport=WebSocket /mcp)",
             MCP_PROTOCOL_VERSION, (int)TOOL_COUNT);
    ESP_LOGI(TAG, "Available tools:");
    for (int i = 0; i < (int)TOOL_COUNT; i++) {
        ESP_LOGI(TAG, "  %s - %s", s_tool_meta[i].name, s_tool_meta[i].description);
    }
    return ESP_OK;
}
