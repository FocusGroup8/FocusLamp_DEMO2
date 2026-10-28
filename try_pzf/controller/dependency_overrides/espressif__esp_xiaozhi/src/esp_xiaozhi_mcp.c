/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <esp_log.h>
#include <esp_check.h>
#include <cJSON.h>
#include "esp_mcp_mgr.h"
#include "esp_xiaozhi_transport.h"
#include "esp_xiaozhi_mcp.h"
#include "mcp_delayed_probe.h"
#include "mcp_vision_client.h"
#include <math.h>

/**
 * @brief MCP transport item structure; uses esp_xiaozhi_transport for MCP.
 */
typedef struct esp_xiaozhi_mcp_item_s {
    esp_xiaozhi_mcp_config_t *config;       /*!< XiaoZhi MCP transport configuration */
    esp_xiaozhi_transport_handle_t transport_handle; /*!< Transport handle (MQTT or WebSocket) */
    esp_mcp_mgr_handle_t handle;            /*!< MCP manager handle */
    char *request_session_id;              /*!< Owned, untruncated server session ID */
    char request_scope[32];                /*!< Bounded MCP engine session key */
    uint32_t request_scope_sequence;
    mcp_delayed_probe_t *probe;
} esp_xiaozhi_mcp_item_t;

static const char *TAG = "ESP_XIAOZHI_MCP";

static esp_xiaozhi_mcp_item_t *s_xiaozhi_mcp = NULL;

static esp_xiaozhi_mcp_item_t *esp_xiaozhi_mcp_get_item_by_mgr_handle(esp_mcp_mgr_handle_t mgr_handle)
{
    esp_xiaozhi_mcp_item_t *mcp_item = (esp_xiaozhi_mcp_item_t *)s_xiaozhi_mcp;
    if (mcp_item && mcp_item->handle == mgr_handle) {
        return mcp_item;
    }

    return NULL;
}

void esp_xiaozhi_mcp_probe_connection(esp_mcp_mgr_handle_t handle, bool online)
{
    esp_xiaozhi_mcp_item_t *item=esp_xiaozhi_mcp_get_item_by_mgr_handle(handle);
    if (item) mcp_delayed_probe_connection(item->probe,online);
}
void esp_xiaozhi_mcp_probe_session_closed(esp_mcp_mgr_handle_t handle)
{
    esp_xiaozhi_mcp_item_t *item=esp_xiaozhi_mcp_get_item_by_mgr_handle(handle);
    if (item) mcp_delayed_probe_session_changed(item->probe);
}
static esp_err_t probe_send(void *context, const char *wire, mcp_probe_guard_t guard, void *guard_context)
{
    esp_xiaozhi_mcp_item_t *item=context;
    return esp_xiaozhi_transport_send_text_guarded(item->transport_handle,wire,guard,guard_context);
}

/* Build a normal CallToolResult, not a second response or MCP task notification. */
static char *probe_response(const cJSON *id, const char *session, const char *text, int error, bool tool_failed)
{
    cJSON *root=cJSON_CreateObject();
    if (!root) return NULL;
    cJSON *payload=cJSON_AddObjectToObject(root,"payload");
    cJSON *copy=cJSON_Duplicate(id,true);
    if (!payload || !copy) { cJSON_Delete(copy); cJSON_Delete(root); return NULL; }
    if (!cJSON_AddItemToObjectCS(payload,"id",copy)) { cJSON_Delete(copy); cJSON_Delete(root); return NULL; }
    bool ok=cJSON_AddStringToObject(root,"session_id",session) &&
            cJSON_AddStringToObject(root,"type","mcp") && cJSON_AddStringToObject(payload,"jsonrpc","2.0");
    cJSON *result=ok ? cJSON_AddObjectToObject(payload,error ? "error" : "result") : NULL;
    if (error) {
        ok=result && cJSON_AddNumberToObject(result,"code",error) && cJSON_AddStringToObject(result,"message",text);
    } else {
        cJSON *content=result ? cJSON_AddArrayToObject(result,"content") : NULL;
        cJSON *entry=content ? cJSON_CreateObject() : NULL;
        if (entry && !cJSON_AddItemToArray(content,entry)) { cJSON_Delete(entry); entry=NULL; }
        ok=entry && cJSON_AddStringToObject(entry,"type","text") && cJSON_AddStringToObject(entry,"text",text) &&
           cJSON_AddBoolToObject(result,"isError",tool_failed);
    }
    char *wire=ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    return wire;
}
static char *vision_work(const char *id_json,const char *session,const char *question,
                          mcp_probe_guard_t current,void *context)
{
    bool failed=true;
    char *answer=mcp_vision_answer(question,current,context,&failed);
    cJSON *id=cJSON_Parse(id_json);
    char *wire=id ? probe_response(id,session,answer ? answer : "识图内存不足。",0,failed) : NULL;
    cJSON_Delete(id);free(answer);
    return wire;
}
static esp_err_t probe_try_handle(esp_xiaozhi_mcp_item_t *item, const char *payload,
                                  const char *session, bool *handled)
{
    *handled=false;
    cJSON *root=cJSON_Parse(payload);
    if (!root) return ESP_OK;
    const cJSON *method=cJSON_GetObjectItem(root,"method"), *params=cJSON_GetObjectItem(root,"params");
    const cJSON *name=cJSON_GetObjectItem(params,"name");
    if (!cJSON_IsString(method) || strcmp(method->valuestring,"tools/call") || !cJSON_IsString(name) ||
        (strcmp(name->valuestring,MCP_PROBE_TOOL_NAME) && strcmp(name->valuestring,MCP_VISION_TOOL_NAME))) {
        cJSON_Delete(root); return ESP_OK;
    }
    *handled=true;
    bool vision=!strcmp(name->valuestring,MCP_VISION_TOOL_NAME);
    const cJSON *id=cJSON_GetObjectItem(root,"id");
    if (!cJSON_IsString(id) && !(cJSON_IsNumber(id) && isfinite(id->valuedouble))) {
        cJSON_Delete(root); return ESP_ERR_INVALID_ARG;
    }
    const cJSON *args=cJSON_GetObjectItem(params,"arguments");
    const cJSON *delay=cJSON_GetObjectItem(args,"delay_ms"), *text=cJSON_GetObjectItem(args,"text");
    const cJSON *question=cJSON_GetObjectItem(args,"question");
    const char *question_text=question && cJSON_IsString(question) ? question->valuestring : "请描述当前摄像头画面中的物体。";
    int error=0;
    const char *message=text && cJSON_IsString(text) ? text->valuestring : "延迟验证完成，校验码7301。";
    int delay_ms=delay && cJSON_IsNumber(delay) ? delay->valueint : 3000;
    if (!session || !*session || strlen(session)>256 || cJSON_GetObjectItem(params,"task") ||
        (args && !cJSON_IsObject(args)) ||
        (!vision && delay && (!cJSON_IsNumber(delay) || (delay->valuedouble!=0 && delay->valuedouble!=3000))) ||
        (!vision && text && (!cJSON_IsString(text) || !*text->valuestring || strlen(text->valuestring)>160)) ||
        (vision && question && (!cJSON_IsString(question) || !*question->valuestring || strlen(question->valuestring)>1000))) {
        error=-32602; message="Invalid tool arguments/current session; no task mode; question <=1000 UTF-8 bytes";
    }
    char *id_json=cJSON_PrintUnformatted(id);
    if (!id_json || strlen(id_json)>128) { cJSON_free(id_json); cJSON_Delete(root); return ESP_ERR_INVALID_ARG; }
    char *wire=probe_response(id,session ? session : "",message,error,false);
    esp_err_t ret=wire ? ESP_OK : ESP_ERR_NO_MEM;
    if (wire && !error) ret=vision ?
        mcp_delayed_probe_submit_work(item->probe,id_json,session,question_text,vision_work) :
        mcp_delayed_probe_submit(item->probe,wire,id_json,session,(uint32_t)delay_ms);
    if (ret==MCP_PROBE_DUPLICATE) ret=ESP_OK;
    else if (wire && (error || ret!=ESP_OK)) {
        if (!error) {
            cJSON_free(wire);
            wire=probe_response(id,session,"Tool busy/unavailable; request not queued",-32000,false);
        }
        ret=wire ? esp_xiaozhi_transport_send_text(item->transport_handle,wire) : ESP_ERR_NO_MEM;
    }
    cJSON_free(wire); cJSON_free(id_json); cJSON_Delete(root);
    return ret;
}

esp_err_t esp_xiaozhi_mcp_handle_message(esp_mcp_mgr_handle_t mgr_handle,
                                         const char *ep_name,
                                         const char *payload,
                                         size_t payload_len,
                                         const char *session_id)
{
    ESP_RETURN_ON_FALSE(ep_name, ESP_ERR_INVALID_ARG, TAG, "Invalid endpoint name");
    ESP_RETURN_ON_FALSE(payload, ESP_ERR_INVALID_ARG, TAG, "Invalid payload");

    esp_xiaozhi_mcp_item_t *mcp_item = esp_xiaozhi_mcp_get_item_by_mgr_handle(mgr_handle);
    ESP_RETURN_ON_FALSE(mcp_item, ESP_ERR_INVALID_ARG, TAG, "Invalid MCP item or handle mismatch");

    /* The engine's session key is bounded; compare full server IDs here instead
     * of letting distinct long IDs collapse into the same truncated key. */
    const char *incoming_session = session_id ? session_id : "";
    esp_err_t ret;
    if (!mcp_item->request_session_id ||
            strcmp(mcp_item->request_session_id, incoming_session) != 0) {
        char *new_session = strdup(incoming_session);
        ESP_RETURN_ON_FALSE(new_session, ESP_ERR_NO_MEM, TAG, "Failed to copy MCP session ID");
        if (mcp_item->request_scope[0]) {
            ret = esp_mcp_mgr_clear_session_state(mgr_handle, mcp_item->request_scope);
            if (ret != ESP_OK) {
                free(new_session);
                ESP_RETURN_ON_ERROR(ret, TAG, "Failed to clear previous MCP session");
            }
        }
        ++mcp_item->request_scope_sequence;
        mcp_delayed_probe_session_changed(mcp_item->probe);
        snprintf(mcp_item->request_scope, sizeof(mcp_item->request_scope),
                 "xiaozhi-%lu", (unsigned long)mcp_item->request_scope_sequence);
        free(mcp_item->request_session_id);
        mcp_item->request_session_id = new_session;
        ESP_LOGI(TAG, "MCP session switched: scope=%s; tools retained", mcp_item->request_scope);
    }
    /* Preserve the existing authenticated context and same-session deduplication.
     * Outgoing responses below still carry the original server session ID. */
    ret = esp_mcp_mgr_set_request_context(mgr_handle, mcp_item->request_scope, NULL, true);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to set MCP request session context");

    bool probe_handled=false;
    ret=probe_try_handle(mcp_item,payload,session_id,&probe_handled);
    if (probe_handled) return ret;

    uint8_t *outbuf = NULL;
    uint16_t outlen = 0;
    ret = esp_mcp_mgr_req_handle(mgr_handle, ep_name, (const uint8_t *)payload, payload_len, &outbuf, &outlen);
    if (ret == ESP_OK && outbuf && outlen > 0) {
        cJSON *response_root = cJSON_CreateObject();
        if (session_id) {
            cJSON_AddStringToObject(response_root, "session_id", session_id);
        }
        cJSON_AddStringToObject(response_root, "type", "mcp");
        cJSON *response_payload = cJSON_Parse((char *)outbuf);
        if (response_payload) {
            cJSON_AddItemToObject(response_root, "payload", response_payload);
            char *json_str = cJSON_PrintUnformatted(response_root);
            if (json_str) {
                esp_xiaozhi_transport_send_text(mcp_item->transport_handle, json_str);
                cJSON_free(json_str);
            } else {
                ESP_LOGE(TAG, "Failed to print MCP response");
            }
        } else {
            ESP_LOGE(TAG, "Failed to parse MCP response payload");
        }
        cJSON_Delete(response_root);
        esp_mcp_mgr_req_destroy_response(mgr_handle, outbuf);
    } else if (ret != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to process MCP message: %s", esp_err_to_name(ret));
    }

    return ESP_OK;
}

esp_err_t esp_xiaozhi_mcp_get_handle(esp_mcp_mgr_handle_t mgr_handle, esp_xiaozhi_transport_handle_t *transport_handle)
{
    ESP_RETURN_ON_FALSE(transport_handle, ESP_ERR_INVALID_ARG, TAG, "Invalid transport handle");

    esp_xiaozhi_mcp_item_t *mcp_item = esp_xiaozhi_mcp_get_item_by_mgr_handle(mgr_handle);
    ESP_RETURN_ON_FALSE(mcp_item, ESP_ERR_INVALID_ARG, TAG, "Invalid MCP item or handle mismatch");

    *transport_handle = mcp_item->transport_handle;

    return ESP_OK;
}

static esp_err_t esp_xiaozhi_mcp_init(esp_mcp_mgr_handle_t handle, esp_mcp_transport_handle_t *transport_handle)
{
    esp_xiaozhi_mcp_item_t *mcp_item = calloc(1, sizeof(esp_xiaozhi_mcp_item_t));
    ESP_RETURN_ON_FALSE(mcp_item, ESP_ERR_NO_MEM, TAG, "Failed to allocate memory for MCP transport item");

    mcp_item->handle = handle;
    mcp_item->config = NULL;

    esp_err_t ret = esp_xiaozhi_transport_init(&mcp_item->transport_handle);
    if (ret != ESP_OK) {
        free(mcp_item);
        return ret;
    }

    mcp_item->probe=mcp_delayed_probe_create(probe_send,mcp_item);
    if (!mcp_item->probe) {
        esp_xiaozhi_transport_deinit(mcp_item->transport_handle);
        free(mcp_item);
        return ESP_ERR_NO_MEM;
    }
    *transport_handle = (esp_mcp_transport_handle_t)mcp_item;
    s_xiaozhi_mcp = mcp_item;

    return ESP_OK;
}

static esp_err_t esp_xiaozhi_mcp_deinit(esp_mcp_transport_handle_t handle)
{
    esp_xiaozhi_mcp_item_t *mcp_item = (esp_xiaozhi_mcp_item_t *)handle;
    ESP_RETURN_ON_FALSE(mcp_item, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");

    /* Join sends first, then stop receive callbacks before freeing their probe. */
    mcp_delayed_probe_drain(mcp_item->probe);
    esp_xiaozhi_transport_deinit(mcp_item->transport_handle);
    mcp_delayed_probe_destroy(mcp_item->probe);
    mcp_item->handle = 0;
    mcp_item->config = NULL;
    if (s_xiaozhi_mcp == mcp_item) {
        s_xiaozhi_mcp = NULL;
    }
    free(mcp_item->request_session_id);
    free(mcp_item);

    return ESP_OK;
}

static esp_err_t esp_xiaozhi_mcp_create_config(const void *config, void **config_out)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "Invalid configuration");

    esp_xiaozhi_mcp_config_t *old_config = (esp_xiaozhi_mcp_config_t *)config;
    esp_xiaozhi_mcp_config_t *new_config = calloc(1, sizeof(esp_xiaozhi_mcp_config_t));
    ESP_RETURN_ON_FALSE(new_config, ESP_ERR_NO_MEM, TAG, "Failed to allocate memory for new configuration");

    new_config->ctx = old_config->ctx;
    new_config->use_mqtt = old_config->use_mqtt;
    new_config->callbacks = old_config->callbacks;

    if (old_config->session_id && old_config->session_id_len > 0) {
        new_config->session_id_len = old_config->session_id_len;
        new_config->session_id = calloc(1, new_config->session_id_len + 1);
        ESP_RETURN_ON_FALSE(new_config->session_id, ESP_ERR_NO_MEM, TAG, "Failed to allocate memory for session ID");
        memcpy(new_config->session_id, old_config->session_id, new_config->session_id_len);
    }

    *config_out = new_config;

    return ESP_OK;
}

static esp_err_t esp_xiaozhi_mcp_delete_config(void *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "Invalid configuration");

    esp_xiaozhi_mcp_config_t *old_config = (esp_xiaozhi_mcp_config_t *)config;
    if (old_config->session_id) {
        free(old_config->session_id);
    }
    free(old_config);

    return ESP_OK;
}

static esp_err_t esp_xiaozhi_mcp_start(esp_mcp_transport_handle_t handle, void *config)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "Invalid configuration");

    esp_xiaozhi_mcp_item_t *mcp_item = (esp_xiaozhi_mcp_item_t *)handle;
    esp_xiaozhi_mcp_config_t *new_config = (esp_xiaozhi_mcp_config_t *)config;

    mcp_item->config = new_config;
    mcp_delayed_probe_resume(mcp_item->probe);

    esp_err_t ret = esp_xiaozhi_transport_start(mcp_item->transport_handle, new_config->use_mqtt, &new_config->callbacks);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to start transport: %s", esp_err_to_name(ret));

    ESP_LOGD(TAG, "MCP transport started with %s", new_config->use_mqtt ? "MQTT" : "WebSocket");

    return ESP_OK;
}

static esp_err_t esp_xiaozhi_mcp_stop(esp_mcp_transport_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");

    esp_xiaozhi_mcp_item_t *mcp_item = (esp_xiaozhi_mcp_item_t *)handle;

    mcp_delayed_probe_drain(mcp_item->probe);
    esp_xiaozhi_transport_stop(mcp_item->transport_handle);
    mcp_item->config = NULL;

    ESP_LOGD(TAG, "MCP transport stopped");

    return ESP_OK;
}

static esp_err_t esp_xiaozhi_mcp_register_endpoint(esp_mcp_transport_handle_t handle, const char *ep_name, void *priv_data)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");
    ESP_RETURN_ON_FALSE(ep_name, ESP_ERR_INVALID_ARG, TAG, "Invalid endpoint name");

    ESP_LOGD(TAG, "MCP endpoint registered: %s", ep_name);

    return ESP_OK;
}

static esp_err_t esp_xiaozhi_mcp_unregister_endpoint(esp_mcp_transport_handle_t handle, const char *ep_name)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");
    ESP_RETURN_ON_FALSE(ep_name, ESP_ERR_INVALID_ARG, TAG, "Invalid endpoint name");

    ESP_LOGD(TAG, "MCP endpoint unregistered: %s", ep_name);

    return ESP_OK;
}

const esp_mcp_transport_t esp_mcp_transport_xiaozhi = {
    .init                = esp_xiaozhi_mcp_init,
    .deinit              = esp_xiaozhi_mcp_deinit,
    .create_config       = esp_xiaozhi_mcp_create_config,
    .delete_config       = esp_xiaozhi_mcp_delete_config,
    .start               = esp_xiaozhi_mcp_start,
    .stop                = esp_xiaozhi_mcp_stop,
    .register_endpoint   = esp_xiaozhi_mcp_register_endpoint,
    .unregister_endpoint = esp_xiaozhi_mcp_unregister_endpoint,
};
