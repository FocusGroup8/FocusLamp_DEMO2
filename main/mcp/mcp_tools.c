/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mcp_tools.h"

#include "cJSON.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "mcp_tools";

#define MCP_PROTOCOL_VERSION "2025-11-25"

/* JSON-RPC error codes */
#define JSONRPC_PARSE_ERROR -32700
#define JSONRPC_INVALID_REQUEST -32600
#define JSONRPC_METHOD_NOT_FOUND -32601
#define JSONRPC_INVALID_PARAMS -32602
#define JSONRPC_INTERNAL_ERROR -32603

/* Tool metadata for tools.list response */
typedef struct {
    const char *name;
    const char *description;
    const char *input_schema; /*!< JSON schema string */
} tool_meta_t;

static const tool_meta_t s_tool_meta[] = {
    {"camera.start", "Start camera capture and streaming",
     "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"},
    {"camera.stop", "Stop camera capture and streaming",
     "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"},
    {"camera.set_quality", "Set JPEG encoding quality (1-100)",
     "{\"type\":\"object\",\"properties\":{\"quality\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":100}},"
     "\"required\":[\"quality\"]}"},
    {"camera.set_fps", "Set target frame rate (1-30)",
     "{\"type\":\"object\",\"properties\":{\"fps\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":30}},\"required\":["
     "\"fps\"]}"},
    {"display.on", "Turn on the display", "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"},
    {"display.off", "Turn off the display", "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"},
    {"display.set_brightness", "Set display backlight brightness (0-100)",
     "{\"type\":\"object\",\"properties\":{\"level\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100}},"
     "\"required\":[\"level\"]}"},
    {"display.show_camera", "Toggle camera preview on local display",
     "{\"type\":\"object\",\"properties\":{\"enable\":{\"type\":\"boolean\"}},\"required\":[\"enable\"]}"},
    {"led.on", "Turn on LED with optional brightness and color temperature",
     "{\"type\":\"object\",\"properties\":{\"brightness\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100},"
     "\"color_temp\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100}},\"additionalProperties\":false}"},
    {"led.off", "Turn off LED", "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"},
    {"led.set_brightness", "Set LED brightness while maintaining color temperature (0-100)",
     "{\"type\":\"object\",\"properties\":{\"brightness\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100}},"
     "\"required\":[\"brightness\"]}"},
    {"led.set_color_temp", "Set LED color temperature (0=warmest, 100=coolest)",
     "{\"type\":\"object\",\"properties\":{\"color_temp\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100}},"
     "\"required\":[\"color_temp\"]}"},
    {"eyes.set_expression",
     "Set expressive eyes expression (neutral/happy/sad/angry/surprised/sleepy/bored/wink_left/wink_right)",
     "{\"type\":\"object\",\"properties\":{\"expression\":{\"type\":\"string\",\"enum\":[\"neutral\",\"happy\",\"sad\","
     "\"angry\",\"surprised\",\"sleepy\",\"bored\",\"wink_left\",\"wink_right\"]}},\"required\":[\"expression\"]}"},
    {"eyes.look_at", "Set eyes look direction",
     "{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\",\"minimum\":-1,\"maximum\":1},"
     "\"y\":{\"type\":\"number\",\"minimum\":-1,\"maximum\":1}},\"required\":[\"x\",\"y\"]}"},
    {"eyes.blink", "Trigger an eye blink", "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"},
    {"eyes.get_expression", "Get current expressive eyes expression",
     "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"},
    {"algorithm.result", "Receive algorithm detection results from main-client",
     "{\"type\":\"object\",\"properties\":{\"focus\":{\"type\":\"object\",\"properties\":{"
     "\"engage_level_name\":{\"type\":\"string\"},\"focus_level_name\":{\"type\":\"string\"},"
     "\"focus_score\":{\"type\":\"number\"}},\"additionalProperties\":true},"
     "\"emotion\":{\"type\":\"string\"},\"fatigue\":{\"type\":\"integer\"},"
     "\"gesture\":{\"type\":\"string\"},\"vlm_game_detector\":{\"type\":\"object\",\"properties\":{"
     "\"judgment\":{\"type\":\"string\"},\"trigger_source\":{\"type\":\"string\"},"
     "\"reason\":{\"type\":\"string\"}},\"additionalProperties\":true}},\"additionalProperties\":true}"},
};

#define TOOL_COUNT (sizeof(s_tool_meta) / sizeof(s_tool_meta[0]))

static mcp_tools_callbacks_t s_callbacks = {0};
static bool s_initialized                = false;

/*---------------------------------------------------------------
 * Internal: Build JSON-RPC error response
 *-------------------------------------------------------------*/
static void build_error_response(char *buf, int buf_size, int id, int code, const char *message)
{
    snprintf(buf, buf_size, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"error\":{\"code\":%d,\"message\":\"%s\"}}", id, code,
             message ? message : "");
}

/*---------------------------------------------------------------
 * Internal: Build JSON-RPC success response
 *-------------------------------------------------------------*/
static void build_success_response(char *buf, int buf_size, int id, const char *result_json)
{
    snprintf(buf, buf_size, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":%s}", id, result_json ? result_json : "null");
}

/*---------------------------------------------------------------
 * Internal: Find tool by name and return callback
 *-------------------------------------------------------------*/
static mcp_tool_cb_t find_tool_callback(const char *name)
{
    if (name == NULL) {
        return NULL;
    }

    if (strcmp(name, "camera.start") == 0)
        return s_callbacks.camera_start;
    if (strcmp(name, "camera.stop") == 0)
        return s_callbacks.camera_stop;
    if (strcmp(name, "camera.set_quality") == 0)
        return s_callbacks.camera_set_quality;
    if (strcmp(name, "camera.set_fps") == 0)
        return s_callbacks.camera_set_fps;
    if (strcmp(name, "display.on") == 0)
        return s_callbacks.display_on;
    if (strcmp(name, "display.off") == 0)
        return s_callbacks.display_off;
    if (strcmp(name, "display.set_brightness") == 0)
        return s_callbacks.display_set_brightness;
    if (strcmp(name, "display.show_camera") == 0)
        return s_callbacks.display_show_camera;
    if (strcmp(name, "led.on") == 0)
        return s_callbacks.led_on;
    if (strcmp(name, "led.off") == 0)
        return s_callbacks.led_off;
    if (strcmp(name, "led.set_brightness") == 0)
        return s_callbacks.led_set_brightness;
    if (strcmp(name, "led.set_color_temp") == 0)
        return s_callbacks.led_set_color_temp;
    if (strcmp(name, "eyes.set_expression") == 0)
        return s_callbacks.eyes_set_expression;
    if (strcmp(name, "eyes.look_at") == 0)
        return s_callbacks.eyes_look_at;
    if (strcmp(name, "eyes.blink") == 0)
        return s_callbacks.eyes_blink;
    if (strcmp(name, "eyes.get_expression") == 0)
        return s_callbacks.eyes_get_expression;
    if (strcmp(name, "algorithm.result") == 0)
        return s_callbacks.algo_result;

    return NULL;
}

/*---------------------------------------------------------------
 * Internal: Handle tools.list method
 *-------------------------------------------------------------*/
static void handle_tools_list(char *buf, int buf_size, int id)
{
    cJSON *root        = cJSON_CreateObject();
    cJSON *tools_array = cJSON_CreateArray();

    for (int i = 0; i < (int)TOOL_COUNT; i++) {
        cJSON *tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", s_tool_meta[i].name);
        cJSON_AddStringToObject(tool, "description", s_tool_meta[i].description);

        /* Parse input schema string to JSON object */
        cJSON *schema = cJSON_Parse(s_tool_meta[i].input_schema);
        if (schema) {
            cJSON_AddItemToObject(tool, "inputSchema", schema);
        }

        cJSON_AddItemToArray(tools_array, tool);
    }

    cJSON_AddItemToObject(root, "tools", tools_array);

    /* Render to buffer */
    char *result_str = cJSON_PrintUnformatted(root);
    if (result_str) {
        build_success_response(buf, buf_size, id, result_str);
        free(result_str);
    } else {
        build_error_response(buf, buf_size, id, JSONRPC_INTERNAL_ERROR, "Failed to build tools list");
    }

    cJSON_Delete(root);
}

/*---------------------------------------------------------------
 * Internal: Handle tools.call method
 *-------------------------------------------------------------*/
static void handle_tools_call(cJSON *request, char *buf, int buf_size, int id)
{
    cJSON *params = cJSON_GetObjectItem(request, "params");
    if (!params) {
        build_error_response(buf, buf_size, id, JSONRPC_INVALID_PARAMS, "Missing params");
        return;
    }

    cJSON *name_item = cJSON_GetObjectItem(params, "name");
    if (!cJSON_IsString(name_item) || !name_item->valuestring) {
        build_error_response(buf, buf_size, id, JSONRPC_INVALID_PARAMS, "Missing or invalid tool name");
        return;
    }

    const char *tool_name = name_item->valuestring;
    cJSON *arguments      = cJSON_GetObjectItem(params, "arguments");

    /* Find tool metadata to verify it exists */
    bool tool_exists = false;
    for (int i = 0; i < (int)TOOL_COUNT; i++) {
        if (strcmp(s_tool_meta[i].name, tool_name) == 0) {
            tool_exists = true;
            break;
        }
    }

    if (!tool_exists) {
        build_error_response(buf, buf_size, id, JSONRPC_METHOD_NOT_FOUND, "Unknown tool");
        return;
    }

    /* Find callback */
    mcp_tool_cb_t cb = find_tool_callback(tool_name);
    if (cb == NULL) {
        build_error_response(buf, buf_size, id, JSONRPC_INTERNAL_ERROR, "Tool not implemented");
        return;
    }

    /* Call tool */
    char tool_response[256] = {0};
    esp_err_t ret           = cb(arguments, tool_response, sizeof(tool_response));

    /* Build MCP-style response with content array */
    cJSON *result        = cJSON_CreateObject();
    cJSON *content_array = cJSON_CreateArray();
    cJSON *content_item  = cJSON_CreateObject();
    cJSON_AddStringToObject(content_item, "type", "text");
    cJSON_AddStringToObject(content_item, "text", (ret == ESP_OK && tool_response[0] != '\0') ? tool_response : "{}");
    cJSON_AddItemToArray(content_array, content_item);
    cJSON_AddItemToObject(result, "content", content_array);
    cJSON_AddBoolToObject(result, "isError", (ret != ESP_OK));

    char *result_str = cJSON_PrintUnformatted(result);
    if (result_str) {
        build_success_response(buf, buf_size, id, result_str);
        free(result_str);
    } else {
        build_error_response(buf, buf_size, id, JSONRPC_INTERNAL_ERROR, "Failed to build response");
    }

    cJSON_Delete(result);
}

/*---------------------------------------------------------------
 * Internal: Handle initialize method
 *-------------------------------------------------------------*/
static void handle_initialize(char *buf, int buf_size, int id)
{
    char result[256];
    snprintf(result, sizeof(result),
             "{\"protocolVersion\":\"%s\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"esp32p4-camera-"
             "mcp\",\"version\":\"1.0.0\"}}",
             MCP_PROTOCOL_VERSION);
    build_success_response(buf, buf_size, id, result);
}

/*---------------------------------------------------------------
 * Public API
 *-------------------------------------------------------------*/
esp_err_t mcp_tools_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_callbacks, 0, sizeof(s_callbacks));
    s_initialized = true;
    ESP_LOGI(TAG, "MCP tools engine initialized (protocol=%s, tools=%d)", MCP_PROTOCOL_VERSION, (int)TOOL_COUNT);
    return ESP_OK;
}

void mcp_tools_deinit(void)
{
    memset(&s_callbacks, 0, sizeof(s_callbacks));
    s_initialized = false;
}

esp_err_t mcp_tools_register_callbacks(const mcp_tools_callbacks_t *callbacks)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!callbacks) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(&s_callbacks, callbacks, sizeof(s_callbacks));
    ESP_LOGI(TAG, "Callbacks registered");
    return ESP_OK;
}

esp_err_t mcp_tools_handle_message(const char *request_json, char *response_buf, int response_buf_size)
{
    if (!s_initialized || !request_json || !response_buf || response_buf_size <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    response_buf[0] = '\0';

    /* Parse JSON */
    cJSON *request = cJSON_Parse(request_json);
    if (!request) {
        build_error_response(response_buf, response_buf_size, 0, JSONRPC_PARSE_ERROR, "Parse error");
        return ESP_ERR_INVALID_ARG;
    }

    /* Extract id (may be number, string, or null) */
    int id         = 0;
    cJSON *id_item = cJSON_GetObjectItem(request, "id");
    if (cJSON_IsNumber(id_item)) {
        id = id_item->valueint;
    }

    /* Extract method */
    cJSON *method_item = cJSON_GetObjectItem(request, "method");
    if (!cJSON_IsString(method_item) || !method_item->valuestring) {
        build_error_response(response_buf, response_buf_size, id, JSONRPC_INVALID_REQUEST, "Missing method");
        cJSON_Delete(request);
        return ESP_ERR_INVALID_ARG;
    }

    const char *method = method_item->valuestring;

    /* Dispatch method */
    if (strcmp(method, "initialize") == 0) {
        handle_initialize(response_buf, response_buf_size, id);
    } else if (strcmp(method, "tools.list") == 0) {
        handle_tools_list(response_buf, response_buf_size, id);
    } else if (strcmp(method, "tools.call") == 0) {
        handle_tools_call(request, response_buf, response_buf_size, id);
    } else if (strcmp(method, "ping") == 0) {
        build_success_response(response_buf, response_buf_size, id, "{}");
    } else {
        build_error_response(response_buf, response_buf_size, id, JSONRPC_METHOD_NOT_FOUND, "Method not found");
        cJSON_Delete(request);
        return ESP_ERR_NOT_SUPPORTED;
    }

    cJSON_Delete(request);
    return ESP_OK;
}
