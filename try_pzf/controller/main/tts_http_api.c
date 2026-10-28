/*
 * tts_http_api.c - HTTP endpoints for remote control from the head/base board
 *
 * Provides:
 *   POST /api/tts/speak  - TTS text injection (played through this speaker)
 *   POST /api/chat/end   - end the current xiaozhi voice conversation
 *   POST /api/detect/phone - enqueue a phone/computer reminder locally
 *   GET  /api/ambient   - fresh local ambient cache (estimated, filtered lux)
 *   POST /api/arm/gesture - serial local executor; accepted != completed
 *   POST /api/arm/stop - bounded soft stop, preserves torque
 *   GET  /api/arm/pose - read-only cached servo feedback, sampled while idle
 *   GET  /api/arm/limits - configured manual-only mechanical intervals
 *   POST /api/arm/servo - one manual servo job; accepted != completed
 *   GET/POST /api/arm/calibration - temporary single-axis calibration session
 *   POST /api/arm/jog - one relative step, calculated from fresh worker feedback
 *
 * The dowm (FocusLamp base) board sends TTS text; the head board can also
 * end an active voice session (long-press on the head light).
 *
 * Request:  POST /api/tts/speak
 * Body:     {"text": "播报的文字内容", "priority": 1}
 * Response: {"ok": true}
 *
 * Request:  POST /api/chat/end
 * Body:     {} (optional, ignored)
 * Response: {"ok": true}
 */

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "websocket_manager.h"
#include "xiaozhi_manager.h"
#include "motion_controller.h"
#include "ambient_service.h"
#include "local_reminder.h"
#include "tts_capture.h"
#include "mcp_vision_client.h"

static const char *TAG = "TTS_API";

/* Max TTS text length */
#define TTS_TEXT_MAX_LEN 256

/*---------------------------------------------------------------
 * HTTP handler: POST /api/tts/speak
 *-------------------------------------------------------------*/
static esp_err_t tts_speak_handler(httpd_req_t *req)
{
    char *body = malloc(req->content_len + 1);
    if (!body) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no memory\"}");
        return ESP_ERR_NO_MEM;
    }

    int received = 0;
    int total = req->content_len;
    if (total < 0) total = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, body + received, total - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            free(body);
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"read failed\"}");
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';

    /* Parse JSON */
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        free(body);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid JSON\"}");
        return ESP_FAIL;
    }

    cJSON *text_item = cJSON_GetObjectItem(root, "text");
    if (!cJSON_IsString(text_item) || !text_item->valuestring) {
        cJSON_Delete(root);
        free(body);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"missing 'text' field\"}");
        return ESP_FAIL;
    }

    const char *text = text_item->valuestring;

    /* Optional priority (default 1) */
    int priority = 1;
    cJSON *prio_item = cJSON_GetObjectItem(root, "priority");
    if (cJSON_IsNumber(prio_item)) {
        priority = prio_item->valueint;
    }

    ESP_LOGI(TAG, "TTS speak: \"%s\" (priority=%d)", text, priority);

    /* Preserve the HTTP contract: ok means accepted, never already played.
     * Exact legacy welcome aliases bypass cloud text injection entirely. */
    bool local_welcome = local_welcome_matches(text);
    esp_err_t ret = local_welcome ? local_welcome_submit() :
                                  xiaozhi_manager_speak(text, priority);

    cJSON_Delete(root);
    free(body);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "%s request rejected: %s", local_welcome ? "Local welcome" : "Cloud speak",
                 esp_err_to_name(ret));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, local_welcome ?
            "{\"ok\":false,\"error\":\"local welcome unavailable\"}" :
            "{\"ok\":false,\"error\":\"xiaozhi speak failed\"}");
        return ESP_OK; /* Don't fail the HTTP request */
    }

    ESP_LOGI(TAG, "%s request accepted", local_welcome ? "Local welcome" : "Cloud speak");
    httpd_resp_set_type(req, "application/json");
    const char *resp = "{\"ok\":true}";
    httpd_resp_send(req, resp, strlen(resp));

    return ESP_OK;
}

static const httpd_uri_t tts_speak_uri = {
    .uri      = "/api/tts/speak",
    .method   = HTTP_POST,
    .handler  = tts_speak_handler,
    .user_ctx = NULL,
};

/*---------------------------------------------------------------
 * HTTP handler: POST /api/chat/end
 *
 * Ends the current xiaozhi voice conversation (closes the audio channel,
 * aborting any ongoing TTS). The wake word engine stays active, so a new
 * conversation can still be started by voice afterwards.
 *-------------------------------------------------------------*/
static esp_err_t chat_end_handler(httpd_req_t *req)
{
    /* Drain the (ignored) request body, if any */
    if (req->content_len > 0) {
        char *body = malloc(req->content_len + 1);
        if (body) {
            int received = 0;
            while (received < req->content_len) {
                int ret = httpd_req_recv(req, body + received, req->content_len - received);
                if (ret <= 0) {
                    if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                        continue;
                    }
                    break;
                }
                received += ret;
            }
            free(body);
        }
    }

    ESP_LOGI(TAG, "Chat end requested");

    /* Cancel local output before any potentially waiting cloud close call. */
    local_welcome_cancel();
    esp_err_t ret = xiaozhi_manager_close_audio_channel();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "xiaozhi_manager_close_audio_channel failed: %s", esp_err_to_name(ret));
    }

    httpd_resp_set_type(req, "application/json");
    const char *resp = "{\"ok\":true}";
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

static const httpd_uri_t chat_end_uri = {
    .uri      = "/api/chat/end",
    .method   = HTTP_POST,
    .handler  = chat_end_handler,
    .user_ctx = NULL,
};

/* L5.0: only successful local enqueue is reported as HTTP 200. */
static esp_err_t detect_phone_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    if (req->content_len == 0 || req->content_len > INT_MAX - 1) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid body length\"}");
        return ESP_FAIL;
    }

    int total = (int)req->content_len;
    char *body = malloc((size_t)total + 1);
    if (!body) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no memory\"}");
        return ESP_ERR_NO_MEM;
    }

    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, body + received, total - received);
        if (ret <= 0) {
            free(body);
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                httpd_resp_set_status(req, "408 Request Timeout");
            } else {
                httpd_resp_set_status(req, "400 Bad Request");
            }
            httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"read failed\"}");
            return ESP_FAIL; /* Close the session if the body was incomplete. */
        }
        received += ret;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid JSON\"}");
        return ESP_FAIL;
    }

    cJSON *source = cJSON_GetObjectItem(root, "source");
    if (!cJSON_IsString(source) || !source->valuestring) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"missing 'source' field\"}");
        return ESP_FAIL;
    }

    const char *text;
    if (strcmp(source->valuestring, "phone") == 0) {
        text = "提醒玩手机";
    } else if (strcmp(source->valuestring, "computer") == 0) {
        text = "提醒玩电脑";
    } else {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"unknown source\"}");
        return ESP_FAIL;
    }
    cJSON_Delete(root);

    esp_err_t ret = xiaozhi_manager_speak(text, 1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Detect reminder enqueue failed: %s", esp_err_to_name(ret));
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"tts enqueue failed\"}");
    }

    ESP_LOGI(TAG, "Detect reminder queued: %s", text);
    httpd_resp_set_status(req, "200 OK");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static const httpd_uri_t detect_phone_uri = {
    .uri = "/api/detect/phone",
    .method = HTTP_POST,
    .handler = detect_phone_handler,
    .user_ctx = NULL,
};

static esp_err_t ambient_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req,"application/json");
    ambient_snapshot_t sample;
    if (ambient_service_get_snapshot(&sample) != ESP_OK || !sample.fresh) {
        httpd_resp_set_status(req,"503 Service Unavailable");
        /* head ignores ok/stale fields: no numeric level may escape here. */
        return httpd_resp_sendstr(req,"{\"ok\":false,\"error\":\"ambient sample unavailable or expired\"}");
    }
    char response[160];
    snprintf(response,sizeof(response),"{\"ok\":true,\"level\":%u,\"lux\":%.2f,\"sampled_at_ms\":%lld,\"age_ms\":%lld}",
             (unsigned)sample.level, (double)sample.lux,
             (long long)sample.last_ok_ms, (long long)sample.age_ms);
    return httpd_resp_sendstr(req,response);
}

static const httpd_uri_t ambient_get_uri = {
    .uri = "/api/ambient",
    .method = HTTP_GET,
    .handler = ambient_get_handler,
    .user_ctx = NULL,
};

/* Read the sampler's cache only. HTTP never starts sampling or owns the bus. */
static esp_err_t arm_pose_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    motion_position_snapshot_t snapshot;
    if (motion_controller_get_position_snapshot(&snapshot) != ESP_OK ||
        !snapshot.sampling_started) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"arm position sampler unavailable\"}");
    }

    cJSON *root = cJSON_CreateObject();
    if (!root ||
        !cJSON_AddBoolToObject(root, "ok", true) ||
        !cJSON_AddBoolToObject(root, "read_only", true) ||
        !cJSON_AddStringToObject(root, "source", "servo-feedback-cache") ||
        !cJSON_AddStringToObject(root, "sampling_mode", "idle_only") ||
        !cJSON_AddBoolToObject(root, "sampling_started", snapshot.sampling_started) ||
        !cJSON_AddBoolToObject(root, "motion_busy", snapshot.motion_busy) ||
        !cJSON_AddNumberToObject(root, "captured_at_ms", snapshot.captured_at_ms) ||
        !cJSON_AddNumberToObject(root, "stale_after_ms", snapshot.stale_after_ms)) {
        goto no_memory;
    }
    cJSON *axes = cJSON_AddArrayToObject(root, "axes");
    if (!axes) goto no_memory;
    for (size_t i = 0; i < MOTION_POSITION_AXIS_COUNT; ++i) {
        const motion_axis_position_snapshot_t *axis = &snapshot.axes[i];
        cJSON *item = cJSON_CreateObject();
        if (!item) goto no_memory;
        if (!cJSON_AddItemToArray(axes, item)) {
            cJSON_Delete(item);
            goto no_memory;
        }
        if (!cJSON_AddNumberToObject(item, "axis", axis->axis) ||
            !cJSON_AddNumberToObject(item, "bus_id", axis->bus_id) ||
            !cJSON_AddStringToObject(item, "protocol", axis->axis == 0 ? "em3" : "lx") ||
            !(axis->has_value ? cJSON_AddNumberToObject(item, "raw", axis->raw) :
                               cJSON_AddNullToObject(item, "raw")) ||
            !cJSON_AddBoolToObject(item, "valid", axis->valid) ||
            !cJSON_AddBoolToObject(item, "stale", axis->stale) ||
            !(axis->has_value && axis->age_ms >= 0 ?
                cJSON_AddNumberToObject(item, "age_ms", axis->age_ms) :
                cJSON_AddNullToObject(item, "age_ms")) ||
            !(axis->attempted ?
                cJSON_AddNumberToObject(item, "last_attempt_at_ms", axis->last_attempt_at_ms) :
                cJSON_AddNullToObject(item, "last_attempt_at_ms")) ||
            !(axis->has_value ?
                cJSON_AddNumberToObject(item, "last_ok_at_ms", axis->last_ok_at_ms) :
                cJSON_AddNullToObject(item, "last_ok_at_ms")) ||
            !cJSON_AddNumberToObject(item, "attempts", axis->attempts) ||
            !cJSON_AddNumberToObject(item, "successes", axis->successes) ||
            !cJSON_AddNumberToObject(item, "failures", axis->failures)) {
            goto no_memory;
        }
    }
    char *response = cJSON_PrintUnformatted(root);
    if (!response) goto no_memory;
    cJSON_Delete(root);
    httpd_resp_set_status(req, "200 OK");
    esp_err_t ret = httpd_resp_sendstr(req, response);
    cJSON_free(response);
    return ret;

no_memory:
    cJSON_Delete(root);
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no memory\"}");
}

static const httpd_uri_t arm_pose_uri = {
    .uri = "/api/arm/pose",
    .method = HTTP_GET,
    .handler = arm_pose_handler,
    .user_ctx = NULL,
};

static esp_err_t arm_manual_error(httpd_req_t *req,const char *status,
                                  const char *code,const char *message)
{
    char response[256];
    httpd_resp_set_type(req,"application/json");
    httpd_resp_set_status(req,status);
    /* Codes/messages are fixed literals supplied by this module. */
    snprintf(response,sizeof(response),
        "{\"ok\":false,\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",code,message);
    return httpd_resp_sendstr(req,response);
}

static esp_err_t arm_limits_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req,"application/json");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    cJSON *root=cJSON_CreateObject();
    if (!root || !cJSON_AddBoolToObject(root,"ok",true) ||
        !cJSON_AddStringToObject(root,"source","manual-servo-limits") ||
        !cJSON_AddNumberToObject(root,"time_ms_min",MOTION_MANUAL_TIME_MS_MIN) ||
        !cJSON_AddNumberToObject(root,"time_ms_max",MOTION_MANUAL_TIME_MS_MAX) ||
        !cJSON_AddNumberToObject(root,"time_ms_default",MOTION_MANUAL_TIME_MS_DEFAULT) ||
        !cJSON_AddNumberToObject(root,"speed_min",MOTION_MANUAL_SPEED_MIN) ||
        !cJSON_AddNumberToObject(root,"speed_max",MOTION_MANUAL_SPEED_MAX) ||
        !cJSON_AddNumberToObject(root,"speed_default",MOTION_MANUAL_SPEED_DEFAULT)) goto no_memory;
    cJSON *axes=cJSON_AddArrayToObject(root,"axes");
    if (!axes) goto no_memory;
    for (int servo_id=1; servo_id<=MOTION_MANUAL_SERVO_COUNT; ++servo_id) {
        motion_manual_servo_limits_t limits;
        if (motion_manual_servo_get_limits(servo_id,&limits)!=ESP_OK) goto no_memory;
        cJSON *item=cJSON_CreateObject();
        if (!item) goto no_memory;
        if (!cJSON_AddItemToArray(axes,item)) { cJSON_Delete(item); goto no_memory; }
        if (!cJSON_AddNumberToObject(item,"servo_id",servo_id) ||
            !cJSON_AddNumberToObject(item,"axis",limits.axis) ||
            !cJSON_AddStringToObject(item,"protocol",limits.axis==0 ? "em3" : "lx") ||
            !cJSON_AddBoolToObject(item,"configured",limits.configured) ||
            !(limits.configured ? cJSON_AddNumberToObject(item,"min",limits.min_position) :
                                   cJSON_AddNullToObject(item,"min")) ||
            !(limits.configured ? cJSON_AddNumberToObject(item,"max",limits.max_position) :
                                   cJSON_AddNullToObject(item,"max"))) goto no_memory;
    }
    char *response=cJSON_PrintUnformatted(root);
    if (!response) goto no_memory;
    cJSON_Delete(root);
    httpd_resp_set_status(req,"200 OK");
    esp_err_t ret=httpd_resp_sendstr(req,response);
    cJSON_free(response);
    return ret;
no_memory:
    cJSON_Delete(root);
    return arm_manual_error(req,"500 Internal Server Error","no_memory","Unable to encode manual limits");
}

static const httpd_uri_t arm_limits_uri={
    .uri="/api/arm/limits", .method=HTTP_GET, .handler=arm_limits_handler,
};

typedef struct { int servo_id,position,time_ms,speed; unsigned fields; } arm_manual_request_t;
#define ARM_MANUAL_SERVO_FIELD 1U
#define ARM_MANUAL_POSITION_FIELD 2U
#define ARM_MANUAL_TIME_FIELD 4U
#define ARM_MANUAL_SPEED_FIELD 8U

/* The endpoint accepts one flat object of finite integer values. Integer-valued
 * JSON numbers such as 1.0 or 1e3 are valid; fractions and bools are not. */
static bool arm_manual_parse(const char *body,arm_manual_request_t *out)
{
    *out=(arm_manual_request_t){0};
    cJSON *root=cJSON_ParseWithOpts(body,NULL,true);
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return false; }
    bool valid=true;
    const cJSON *item;
    cJSON_ArrayForEach(item,root) {
        unsigned field;
        int *value;
        if (!strcmp(item->string,"servo_id")) { field=ARM_MANUAL_SERVO_FIELD; value=&out->servo_id; }
        else if (!strcmp(item->string,"position")) { field=ARM_MANUAL_POSITION_FIELD; value=&out->position; }
        else if (!strcmp(item->string,"time_ms")) { field=ARM_MANUAL_TIME_FIELD; value=&out->time_ms; }
        else if (!strcmp(item->string,"speed")) { field=ARM_MANUAL_SPEED_FIELD; value=&out->speed; }
        else { valid=false; break; }
        if ((out->fields & field) || !cJSON_IsNumber(item) ||
            !isfinite(item->valuedouble) || item->valuedouble<0 ||
            item->valuedouble>INT_MAX || item->valuedouble!=(int)item->valuedouble) {
            valid=false; break;
        }
        out->fields|=field;
        *value=(int)item->valuedouble;
    }
    cJSON_Delete(root);
    return valid && (out->fields & (ARM_MANUAL_SERVO_FIELD|ARM_MANUAL_POSITION_FIELD))==
                   (ARM_MANUAL_SERVO_FIELD|ARM_MANUAL_POSITION_FIELD);
}

static esp_err_t arm_servo_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req,"application/json");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    char body[129];
    if (req->content_len==0 || req->content_len>sizeof(body)-1) {
        httpd_resp_set_hdr(req,"Connection","close");
        arm_manual_error(req,"400 Bad Request","invalid_body","Body must contain 1 to 128 bytes");
        return ESP_FAIL;
    }
    size_t received=0;
    while (received<req->content_len) {
        int n=httpd_req_recv(req,body+received,req->content_len-received);
        if (n<=0) {
            httpd_resp_set_hdr(req,"Connection","close");
            arm_manual_error(req,n==HTTPD_SOCK_ERR_TIMEOUT ? "408 Request Timeout" : "400 Bad Request",
                             "incomplete_body","Unable to read the complete body");
            return ESP_FAIL;
        }
        received+=(size_t)n;
    }
    body[received]='\0';
    arm_manual_request_t command;
    if (memchr(body,'\0',received) || !arm_manual_parse(body,&command) ||
        command.servo_id<1 || command.servo_id>MOTION_MANUAL_SERVO_COUNT) {
        return arm_manual_error(req,"400 Bad Request","invalid_request",
            "Expected servo_id 1..5, integer position and optional protocol-specific parameter");
    }
    if (command.servo_id==4) {
        if (command.fields & ARM_MANUAL_TIME_FIELD)
            return arm_manual_error(req,"400 Bad Request","invalid_parameter","EM3 accepts speed; time_ms is not allowed");
        if (!(command.fields & ARM_MANUAL_SPEED_FIELD)) command.speed=MOTION_MANUAL_SPEED_DEFAULT;
        if (command.speed<MOTION_MANUAL_SPEED_MIN || command.speed>MOTION_MANUAL_SPEED_MAX)
            return arm_manual_error(req,"400 Bad Request","invalid_speed","EM3 speed must be 1..500");
    } else {
        if (command.fields & ARM_MANUAL_SPEED_FIELD)
            return arm_manual_error(req,"400 Bad Request","invalid_parameter","LX accepts time_ms; speed is not allowed");
        if (!(command.fields & ARM_MANUAL_TIME_FIELD)) command.time_ms=MOTION_MANUAL_TIME_MS_DEFAULT;
        if (command.time_ms<MOTION_MANUAL_TIME_MS_MIN || command.time_ms>MOTION_MANUAL_TIME_MS_MAX)
            return arm_manual_error(req,"400 Bad Request","invalid_time_ms","LX time_ms must be 500..10000");
    }
    motion_controller_status_t state;
    if (motion_controller_get_status(&state)!=ESP_OK)
        return arm_manual_error(req,"503 Service Unavailable","unready","Motion controller is not ready");
    motion_manual_servo_limits_t limits;
    motion_manual_servo_get_limits(command.servo_id,&limits);
    if (!limits.configured)
        return arm_manual_error(req,"409 Conflict","limits_unconfigured","This servo has no calibrated manual interval");
    if (command.position<limits.min_position || command.position>limits.max_position)
        return arm_manual_error(req,"400 Bad Request","position_out_of_range","Position is outside the configured manual interval");
    esp_err_t ret=motion_manual_servo_async(command.servo_id,command.position,command.time_ms,command.speed);
    if (ret!=ESP_OK) {
        if (ret==ESP_ERR_INVALID_STATE || ret==ESP_ERR_TIMEOUT)
            return arm_manual_error(req,"409 Conflict","busy","Motion executor is busy; no manual request was queued");
        if (ret==ESP_ERR_NOT_SUPPORTED)
            return arm_manual_error(req,"409 Conflict","limits_unconfigured","This servo has no calibrated manual interval");
        if (ret==ESP_ERR_INVALID_ARG)
            return arm_manual_error(req,"400 Bad Request","invalid_request","Manual target or protocol-specific parameter is invalid");
        return arm_manual_error(req,"500 Internal Server Error","submit_failed","Unable to create the manual worker job");
    }
    char response[192];
    snprintf(response,sizeof(response),
        "{\"ok\":true,\"accepted\":true,\"completed\":false,\"servo_id\":%d,\"axis\":%d,\"position\":%d,\"status\":\"accepted\"}",
        command.servo_id,limits.axis,command.position);
    httpd_resp_set_status(req,"202 Accepted");
    return httpd_resp_sendstr(req,response);
}

static const httpd_uri_t arm_servo_uri={
    .uri="/api/arm/servo", .method=HTTP_POST, .handler=arm_servo_handler,
};

/* These two arm endpoints use a small bounded body; old TTS handlers are unchanged. */
static esp_err_t arm_read_body(httpd_req_t *req,char body[129],bool required)
{
    httpd_resp_set_type(req,"application/json");
    if (req->content_len>128 || (required && req->content_len==0)) {
        httpd_resp_set_status(req,req->content_len>128 ? "413 Payload Too Large" : "400 Bad Request");
        httpd_resp_set_hdr(req,"Connection","close");
        httpd_resp_sendstr(req,"{\"ok\":false,\"error\":\"invalid body length\"}");
        return ESP_FAIL;
    }
    size_t done=0;
    while (done<req->content_len) {
        int n=httpd_req_recv(req,body+done,req->content_len-done);
        if (n<=0) {
            httpd_resp_set_status(req,n==HTTPD_SOCK_ERR_TIMEOUT ? "408 Request Timeout" : "400 Bad Request");
            httpd_resp_set_hdr(req,"Connection","close");
            httpd_resp_sendstr(req,"{\"ok\":false,\"error\":\"incomplete body\"}");
            return ESP_FAIL;
        }
        done+=(size_t)n;
    }
    body[done]='\0';
    if (memchr(body,'\0',done)) {
        httpd_resp_set_status(req,"400 Bad Request");
        httpd_resp_sendstr(req,"{\"ok\":false,\"error\":\"invalid body\"}");
        return ESP_FAIL;
    }
    return ESP_OK;
}
static esp_err_t arm_response(httpd_req_t *req,esp_err_t ret,const char *success)
{
    if (ret==ESP_OK) return httpd_resp_sendstr(req,success);
    motion_controller_status_t state;
    bool ready=motion_controller_get_status(&state)==ESP_OK;
    httpd_resp_set_status(req,!ready ? "503 Service Unavailable" :
        ret==ESP_ERR_INVALID_ARG ? "400 Bad Request" :
        ret==ESP_ERR_TIMEOUT || ret==ESP_ERR_INVALID_STATE ? "409 Conflict" : "500 Internal Server Error");
    char response[96];
    snprintf(response,sizeof(response),"{\"ok\":false,\"error\":\"%s\"}",esp_err_to_name(ret));
    return httpd_resp_sendstr(req,response);
}

/* Calibration requests never write permanent limits or perform UART work here. */
typedef struct {
    unsigned fields;
    int action,servo_id,min,max,step,time_ms,speed,direction;
    uint32_t session_id;
} arm_calibration_request_t;
enum {
    CAL_ACTION=1U, CAL_SERVO=2U, CAL_MIN=4U, CAL_MAX=8U, CAL_STEP=16U,
    CAL_TIME=32U, CAL_SPEED=64U, CAL_CONFIRMED=128U, CAL_SESSION=256U, CAL_DIRECTION=512U,
};

static esp_err_t arm_calibration_read_body(httpd_req_t *req,char body[257])
{
    httpd_resp_set_type(req,"application/json");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    if (req->content_len==0 || req->content_len>256) {
        httpd_resp_set_hdr(req,"Connection","close");
        arm_manual_error(req,req->content_len>256 ? "413 Payload Too Large" : "400 Bad Request",
                         "invalid_request","Expected a JSON body of 1..256 bytes");
        return ESP_FAIL;
    }
    size_t done=0;
    while (done<req->content_len) {
        int n=httpd_req_recv(req,body+done,req->content_len-done);
        if (n<=0) {
            httpd_resp_set_hdr(req,"Connection","close");
            arm_manual_error(req,n==HTTPD_SOCK_ERR_TIMEOUT ? "408 Request Timeout" : "400 Bad Request",
                             "invalid_request","Incomplete calibration request");
            return ESP_FAIL;
        }
        done+=(size_t)n;
    }
    body[done]='\0';
    if (memchr(body,'\0',done)) {
        arm_manual_error(req,"400 Bad Request","invalid_request","Embedded NUL is not allowed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool arm_calibration_parse(const char *body,arm_calibration_request_t *out)
{
    *out=(arm_calibration_request_t){0};
    /* cJSON strings are NUL-terminated: escaped NULs must not hide a suffix
     * behind a recognized field/action when compared with strcmp(). */
    if (strstr(body,"\\u0000")) return false;
    cJSON *root=cJSON_ParseWithOpts(body,NULL,true);
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return false; }
    bool valid=true;
    const cJSON *item;
    cJSON_ArrayForEach(item,root) {
        unsigned field; int *value=NULL;
        if (!strcmp(item->string,"action")) field=CAL_ACTION;
        else if (!strcmp(item->string,"confirmed")) field=CAL_CONFIRMED;
        else if (!strcmp(item->string,"session_id")) field=CAL_SESSION;
        else if (!strcmp(item->string,"servo_id")) { field=CAL_SERVO; value=&out->servo_id; }
        else if (!strcmp(item->string,"min")) { field=CAL_MIN; value=&out->min; }
        else if (!strcmp(item->string,"max")) { field=CAL_MAX; value=&out->max; }
        else if (!strcmp(item->string,"step")) { field=CAL_STEP; value=&out->step; }
        else if (!strcmp(item->string,"time_ms")) { field=CAL_TIME; value=&out->time_ms; }
        else if (!strcmp(item->string,"speed")) { field=CAL_SPEED; value=&out->speed; }
        else if (!strcmp(item->string,"direction")) { field=CAL_DIRECTION; value=&out->direction; }
        else { valid=false; break; }
        if (out->fields & field) { valid=false; break; }
        out->fields|=field;
        if (field==CAL_ACTION) {
            if (!cJSON_IsString(item)) { valid=false; break; }
            if (!strcmp(item->valuestring,"begin")) out->action=1;
            else if (!strcmp(item->valuestring,"end")) out->action=2;
            else { valid=false; break; }
        } else if (field==CAL_CONFIRMED) {
            if (!cJSON_IsTrue(item)) { valid=false; break; }
        } else if (field==CAL_SESSION) {
            double number=item->valuedouble;
            if (!cJSON_IsNumber(item) || !isfinite(number) || number<1 || number>UINT32_MAX ||
                number!=(double)(uint32_t)number) { valid=false; break; }
            out->session_id=(uint32_t)number;
        } else {
            double number=item->valuedouble;
            if (!cJSON_IsNumber(item) || !isfinite(number) || number<(field==CAL_DIRECTION ? -1 : 0) ||
                number>INT_MAX || number!=(int)number) { valid=false; break; }
            *value=(int)number;
        }
    }
    cJSON_Delete(root);
    return valid;
}

static const char *arm_action_status_name(motion_action_status_t status)
{
    switch(status) {
    case ACTION_STATUS_IDLE: return "idle";
    case ACTION_STATUS_RUNNING: return "running";
    case ACTION_STATUS_COMPLETED: return "completed";
    case ACTION_STATUS_INTERRUPTED: return "interrupted";
    case ACTION_STATUS_FAILED: return "failed";
    case ACTION_STATUS_UNCONFIRMED: return "unconfirmed";
    default: return "unknown";
    }
}

static esp_err_t arm_calibration_status_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req,"application/json");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    motion_calibration_snapshot_t state;
    if (motion_calibration_get_snapshot(&state)!=ESP_OK)
        return arm_manual_error(req,"503 Service Unavailable","unready","Calibration executor unavailable");
    cJSON *root=cJSON_CreateObject();
    if (!root || !cJSON_AddBoolToObject(root,"ok",true) ||
        !cJSON_AddStringToObject(root,"source","arm-calibration") ||
        !cJSON_AddBoolToObject(root,"active",state.active) ||
        !cJSON_AddBoolToObject(root,"ready",state.ready) ||
        !cJSON_AddBoolToObject(root,"motion_busy",state.motion_busy) ||
        !cJSON_AddNumberToObject(root,"session_id",state.session_id) ||
        !cJSON_AddNumberToObject(root,"stale_after_ms",MOTION_POSITION_STALE_AFTER_MS) ||
        !cJSON_AddStringToObject(root,"action_status",arm_action_status_name(state.action_status)) ||
        !cJSON_AddStringToObject(root,"last_error",esp_err_to_name(state.last_error)) ||
        !(state.reason ? cJSON_AddStringToObject(root,"reason",state.reason) : cJSON_AddNullToObject(root,"reason")) ||
        !(state.reference_valid ? cJSON_AddNumberToObject(root,"reference_raw",state.reference_raw) : cJSON_AddNullToObject(root,"reference_raw")) ||
        !(state.has_observations ? cJSON_AddNumberToObject(root,"observed_min",state.observed_min) : cJSON_AddNullToObject(root,"observed_min")) ||
        !(state.has_observations ? cJSON_AddNumberToObject(root,"observed_max",state.observed_max) : cJSON_AddNullToObject(root,"observed_max")) ||
        !(state.has_target ? cJSON_AddNumberToObject(root,"target_raw",state.target_raw) : cJSON_AddNullToObject(root,"target_raw"))) goto no_memory;
    if (state.config.servo_id>=1 && state.config.servo_id<=5 && state.session_id!=0) {
        int axis=state.config.servo_id==4 ? 0 : state.config.servo_id==5 ? 4 : state.config.servo_id;
        cJSON *config=cJSON_AddObjectToObject(root,"config");
        cJSON *feedback=cJSON_AddObjectToObject(root,"feedback");
        const motion_axis_position_snapshot_t *p=&state.feedback;
        if (!config || !feedback ||
            !cJSON_AddNumberToObject(config,"servo_id",state.config.servo_id) ||
            !cJSON_AddNumberToObject(config,"axis",axis) ||
            !cJSON_AddStringToObject(config,"protocol",axis==0 ? "em3" : "lx") ||
            !cJSON_AddNumberToObject(config,"min",state.config.min_position) ||
            !cJSON_AddNumberToObject(config,"max",state.config.max_position) ||
            !cJSON_AddNumberToObject(config,"step",state.config.step) ||
            !cJSON_AddNumberToObject(config,"time_ms",state.config.time_ms) ||
            !cJSON_AddNumberToObject(config,"speed",state.config.speed) ||
            !cJSON_AddNumberToObject(feedback,"axis",p->axis) ||
            !cJSON_AddNumberToObject(feedback,"bus_id",p->bus_id) ||
            !cJSON_AddStringToObject(feedback,"protocol",axis==0 ? "em3" : "lx") ||
            !(p->has_value ? cJSON_AddNumberToObject(feedback,"raw",p->raw) : cJSON_AddNullToObject(feedback,"raw")) ||
            !cJSON_AddBoolToObject(feedback,"valid",p->valid) ||
            !cJSON_AddBoolToObject(feedback,"stale",p->stale) ||
            !(p->has_value && p->age_ms>=0 ? cJSON_AddNumberToObject(feedback,"age_ms",p->age_ms) : cJSON_AddNullToObject(feedback,"age_ms")) ||
            !(p->attempted ? cJSON_AddNumberToObject(feedback,"last_attempt_at_ms",p->last_attempt_at_ms) : cJSON_AddNullToObject(feedback,"last_attempt_at_ms")) ||
            !(p->has_value ? cJSON_AddNumberToObject(feedback,"last_ok_at_ms",p->last_ok_at_ms) : cJSON_AddNullToObject(feedback,"last_ok_at_ms")) ||
            !cJSON_AddNumberToObject(feedback,"attempts",p->attempts) ||
            !cJSON_AddNumberToObject(feedback,"successes",p->successes) ||
            !cJSON_AddNumberToObject(feedback,"failures",p->failures)) goto no_memory;
    } else if (!cJSON_AddNullToObject(root,"config") || !cJSON_AddNullToObject(root,"feedback")) goto no_memory;
    char *response=cJSON_PrintUnformatted(root);
    if (!response) goto no_memory;
    cJSON_Delete(root);
    esp_err_t ret=httpd_resp_sendstr(req,response); cJSON_free(response); return ret;
no_memory:
    cJSON_Delete(root);
    return arm_manual_error(req,"500 Internal Server Error","no_memory","Unable to encode calibration status");
}

static esp_err_t arm_calibration_submit_error(httpd_req_t *req,esp_err_t ret)
{
    if (ret==ESP_ERR_NOT_FOUND)
        return arm_manual_error(req,"409 Conflict","invalid_session","Calibration session has changed");
    if (ret==ESP_ERR_INVALID_ARG)
        return arm_manual_error(req,"400 Bad Request","invalid_config","Invalid candidate interval, step or protocol-specific rate");
    if (ret==ESP_ERR_INVALID_STATE || ret==ESP_ERR_TIMEOUT)
        return arm_manual_error(req,"409 Conflict","busy","Executor busy; no request was queued");
    return arm_manual_error(req,"500 Internal Server Error","submit_failed","Unable to submit calibration request");
}

static esp_err_t arm_calibration_handler(httpd_req_t *req)
{
    char body[257]; arm_calibration_request_t command;
    if (arm_calibration_read_body(req,body)!=ESP_OK) return ESP_FAIL;
    if (!arm_calibration_parse(body,&command))
        return arm_manual_error(req,"400 Bad Request","invalid_request","Invalid calibration JSON fields");
    motion_calibration_snapshot_t state;
    if (motion_calibration_get_snapshot(&state)!=ESP_OK)
        return arm_manual_error(req,"503 Service Unavailable","unready","Calibration executor unavailable");
    if (command.action==2 && command.fields==(CAL_ACTION|CAL_SESSION)) {
        esp_err_t ret=motion_calibration_end(command.session_id);
        if (ret!=ESP_OK) return arm_calibration_submit_error(req,ret);
        return httpd_resp_sendstr(req,"{\"ok\":true,\"ended\":true}");
    }
    unsigned required=CAL_ACTION|CAL_SERVO|CAL_MIN|CAL_MAX|CAL_STEP|CAL_CONFIRMED|
        (command.servo_id==4 ? CAL_SPEED : CAL_TIME);
    if (command.action!=1 || command.fields!=required)
        return arm_manual_error(req,"400 Bad Request","invalid_request","Begin requires an explicit candidate interval, step, confirmation and protocol-specific rate");
    if (state.active) return arm_manual_error(req,"409 Conflict","active","End the active calibration before changing its configuration");
    motion_calibration_config_t config={.servo_id=command.servo_id,.min_position=command.min,
        .max_position=command.max,.step=command.step,.time_ms=command.time_ms,.speed=command.speed};
    uint32_t session_id=0;
    esp_err_t ret=motion_calibration_begin_async(&config,&session_id);
    if (ret!=ESP_OK) return arm_calibration_submit_error(req,ret);
    char response[128];
    snprintf(response,sizeof(response),"{\"ok\":true,\"accepted\":true,\"completed\":false,\"session_id\":%lu}",(unsigned long)session_id);
    httpd_resp_set_status(req,"202 Accepted");
    return httpd_resp_sendstr(req,response);
}

static esp_err_t arm_jog_handler(httpd_req_t *req)
{
    char body[257]; arm_calibration_request_t command;
    if (arm_calibration_read_body(req,body)!=ESP_OK) return ESP_FAIL;
    if (!arm_calibration_parse(body,&command) || command.fields!=(CAL_SESSION|CAL_DIRECTION) ||
        (command.direction!=-1 && command.direction!=1))
        return arm_manual_error(req,"400 Bad Request","invalid_request","Expected a session_id and direction -1 or 1");
    motion_calibration_snapshot_t state;
    if (motion_calibration_get_snapshot(&state)!=ESP_OK)
        return arm_manual_error(req,"503 Service Unavailable","unready","Calibration executor unavailable");
    if (!state.active || state.session_id!=command.session_id)
        return arm_manual_error(req,"409 Conflict","invalid_session","Calibration session has changed");
    if (!state.ready) return arm_manual_error(req,"409 Conflict","not_ready","Reference acquisition did not complete successfully");
    esp_err_t ret=motion_calibration_jog_async(command.session_id,command.direction);
    if (ret!=ESP_OK) return arm_calibration_submit_error(req,ret);
    char response[160];
    snprintf(response,sizeof(response),"{\"ok\":true,\"accepted\":true,\"completed\":false,\"session_id\":%lu,\"direction\":%d}",(unsigned long)command.session_id,command.direction);
    httpd_resp_set_status(req,"202 Accepted");
    return httpd_resp_sendstr(req,response);
}

static const httpd_uri_t arm_calibration_status_uri={
    .uri="/api/arm/calibration", .method=HTTP_GET, .handler=arm_calibration_status_handler,
};
static const httpd_uri_t arm_calibration_uri={
    .uri="/api/arm/calibration", .method=HTTP_POST, .handler=arm_calibration_handler,
};
static const httpd_uri_t arm_jog_uri={
    .uri="/api/arm/jog", .method=HTTP_POST, .handler=arm_jog_handler,
};

static esp_err_t arm_gesture_handler(httpd_req_t *req)
{
    char body[129];
    if (arm_read_body(req,body,true)!=ESP_OK) return ESP_FAIL;
    cJSON *root=cJSON_ParseWithOpts(body,NULL,true);
    const cJSON *gesture=cJSON_GetObjectItemCaseSensitive(root,"gesture");
    if (!cJSON_IsObject(root) || !cJSON_IsString(gesture) ||
        (strcmp(gesture->valuestring,"thumb_up") && strcmp(gesture->valuestring,"thumb_down"))) {
        cJSON_Delete(root);
        httpd_resp_set_status(req,"400 Bad Request");
        return httpd_resp_sendstr(req,"{\"ok\":false,\"error\":\"unknown gesture\"}");
    }
    esp_err_t ret=motion_gesture_async(gesture->valuestring);
    ESP_LOGI(TAG,"Arm gesture %s: %s (accepted != completed)",gesture->valuestring,esp_err_to_name(ret));
    cJSON_Delete(root);
    return arm_response(req,ret,"{\"ok\":true,\"status\":\"accepted\"}");
}

static const httpd_uri_t arm_gesture_uri = {
    .uri = "/api/arm/gesture",
    .method = HTTP_POST,
    .handler = arm_gesture_handler,
    .user_ctx = NULL,
};

/*---------------------------------------------------------------
 * Init
 *-------------------------------------------------------------*/

static esp_err_t arm_stop_handler(httpd_req_t *req)
{
    char body[129];
    if (arm_read_body(req,body,false)!=ESP_OK) return ESP_FAIL;
    return arm_response(req,motion_action_stop(),
        "{\"ok\":true,\"status\":\"soft_stopped\"}");
}
static const httpd_uri_t arm_stop_uri={
    .uri="/api/arm/stop", .method=HTTP_POST, .handler=arm_stop_handler,
};

static esp_err_t vision_service_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req,"application/json");
    if (req->content_len<=0 || req->content_len>256) {
        httpd_resp_set_status(req,"400 Bad Request");
        return httpd_resp_sendstr(req,"{\"ok\":false,\"error\":\"invalid service URL body\"}");
    }
    char body[257];int received=0;
    while (received<req->content_len) {
        int n=httpd_req_recv(req,body+received,req->content_len-received);
        if (n<=0) {
            httpd_resp_set_status(req,"400 Bad Request");
            return httpd_resp_sendstr(req,"{\"ok\":false,\"error\":\"body incomplete\"}");
        }
        received+=n;
    }
    body[received]=0;
    cJSON *json=cJSON_Parse(body),*url=cJSON_GetObjectItemCaseSensitive(json,"url");
    esp_err_t ret=cJSON_IsString(url) ? mcp_vision_set_url(url->valuestring) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(json);
    if (ret!=ESP_OK) {
        httpd_resp_set_status(req,"400 Bad Request");
        return httpd_resp_sendstr(req,"{\"ok\":false,\"error\":\"expected private LAN IPv4 HTTP origin\"}");
    }
    char registered[128],response[180];mcp_vision_get_url(registered,sizeof(registered));
    snprintf(response,sizeof(response),"{\"ok\":true,\"url\":\"%s\"}",registered);
    return httpd_resp_sendstr(req,response);
}
static const httpd_uri_t vision_service_uri={
    .uri="/api/vision/service",.method=HTTP_POST,.handler=vision_service_handler,
};

esp_err_t tts_http_api_init(void)
{
    if (!ws_manager_server_is_running()) {
        ESP_LOGE(TAG, "WebSocket server not running, cannot register URI");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ws_manager_server_register_uri(&tts_speak_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /api/tts/speak: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = ws_manager_server_register_uri(&chat_end_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /api/chat/end: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = ws_manager_server_register_uri(&detect_phone_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /api/detect/phone: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = ws_manager_server_register_uri(&ambient_get_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /api/ambient: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = ws_manager_server_register_uri(&arm_gesture_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /api/arm/gesture: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = ws_manager_server_register_uri(&arm_stop_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /api/arm/stop: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = ws_manager_server_register_uri(&arm_pose_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /api/arm/pose: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = ws_manager_server_register_uri(&arm_limits_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /api/arm/limits: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = ws_manager_server_register_uri(&arm_servo_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /api/arm/servo: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = ws_manager_server_register_uri(&arm_calibration_status_uri);
    if (ret != ESP_OK) return ret;
    ret = ws_manager_server_register_uri(&arm_calibration_uri);
    if (ret != ESP_OK) return ret;
    ret = ws_manager_server_register_uri(&arm_jog_uri);
    if (ret != ESP_OK) return ret;

    ret=ws_manager_server_register_uri(&vision_service_uri);
    if (ret!=ESP_OK) return ret;

    ESP_LOGI(TAG, "TTS API endpoints registered: POST /api/tts/speak, POST /api/chat/end");
    ESP_LOGI(TAG, "Local endpoints registered: POST /api/detect/phone, GET /api/ambient, POST /api/arm/gesture, POST /api/arm/stop, GET /api/arm/pose, GET /api/arm/limits, POST /api/arm/servo");
    ret = tts_capture_http_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Optional TTS capture unavailable: %s (existing endpoints remain registered)", esp_err_to_name(ret));
    }
    return ESP_OK;
}
