#include "mode_http_api.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "controller_modes.h"
#include "demo_behaviors.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "websocket_manager.h"

static const char *TAG = "MODE_API";

static esp_err_t mode_status_response(httpd_req_t *req)
{
    controller_modes_status_t state;
    esp_err_t ret = controller_modes_get_status(&state);
    httpd_resp_set_type(req, "application/json");
    if (ret != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"modes not ready\",\"state_only\":true}");
    }

    demo_behaviors_status_t demo;
    demo_behaviors_get_status(&demo);
    bool same = demo.started && demo.revision == state.revision;
    char response[640];
    snprintf(response, sizeof(response),
        "{\"ok\":true,\"mode\":\"%s\",\"app_state\":%u,"
        "\"duration_sec\":%lu,\"remaining_sec\":%lu,\"voice_active\":%s,"
        "\"paused\":%s,\"completed\":%s,\"state_only\":%s,"
        "\"demo_worker\":%s,\"companion_local_ready\":%s,\"head_sync_ready\":%s,"
        "\"radar_fresh\":%s,\"radar_present\":%s,\"sedentary_sec\":%u,\"sedentary_pending\":%s}",
        controller_modes_name(state.mode), (unsigned)state.mode,
        (unsigned long)state.duration_sec, (unsigned long)state.remaining_sec,
        state.voice_active ? "true" : "false", state.paused ? "true" : "false",
        state.completed ? "true" : "false",
        state.mode == APP_STATE_COMPANION && demo.started ? "false" : "true",
        demo.started ? "true" : "false", same && demo.companion_local_ready ? "true" : "false",
        same && demo.head_sync_ready ? "true" : "false", same && demo.radar_fresh ? "true" : "false",
        same && demo.radar_present ? "true" : "false", same ? (unsigned)demo.sedentary_sec : 0U,
        same && demo.sedentary_pending ? "true" : "false");
    return httpd_resp_sendstr(req, response);
}

static esp_err_t mode_post_handler(httpd_req_t *req)
{
    /* No heap-backed request body and no unbounded timeout retry. */
    char body[193];
    httpd_resp_set_type(req, "application/json");
    if (req->content_len == 0 || req->content_len >= sizeof(body)) {
        httpd_resp_set_status(req, req->content_len == 0 ?
                             "400 Bad Request" : "413 Payload Too Large");
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid body length\"}");
        return ESP_FAIL;
    }
    size_t received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) {
            httpd_resp_set_status(req, n == HTTPD_SOCK_ERR_TIMEOUT ?
                                 "408 Request Timeout" : "400 Bad Request");
            httpd_resp_set_hdr(req, "Connection", "close");
            httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"incomplete body\"}");
            return ESP_FAIL;
        }
        received += (size_t)n;
    }
    body[received] = '\0';
    if (memchr(body, '\0', received)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid body\"}");
    }

    cJSON *root = cJSON_ParseWithOpts(body, NULL, true);
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
    if (!cJSON_IsObject(root) || !cJSON_IsString(mode)) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"missing or invalid mode\"}");
    }

    esp_err_t ret;
    if (strcmp(mode->valuestring, "focus") == 0) {
        const cJSON *duration = cJSON_GetObjectItemCaseSensitive(root, "duration");
        uint32_t minutes = 30;
        if (duration) {
            double value = duration->valuedouble;
            if (!cJSON_IsNumber(duration) || !isfinite(value) || value < 1 ||
                value > 180 || value != (double)duration->valueint) {
                cJSON_Delete(root);
                httpd_resp_set_status(req, "400 Bad Request");
                return httpd_resp_sendstr(req,
                    "{\"ok\":false,\"error\":\"duration must be integer minutes 1..180\"}");
            }
            minutes = (uint32_t)duration->valueint;
        }
        ret = controller_modes_focus_start(minutes);
    } else if (strcmp(mode->valuestring, "companion") == 0) {
        ret = controller_modes_companion_start();
    } else if (strcmp(mode->valuestring, "idle") == 0) {
        ret = controller_modes_stop();
    } else {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"unknown mode\"}");
    }
    cJSON_Delete(root);

    if (ret != ESP_OK) {
        controller_modes_status_t state;
        bool ready = controller_modes_get_status(&state) == ESP_OK;
        httpd_resp_set_status(req, !ready ? "503 Service Unavailable" :
            ret == ESP_ERR_INVALID_ARG ? "400 Bad Request" :
            ret == ESP_ERR_INVALID_STATE ? "409 Conflict" : "500 Internal Server Error");
        char response[128];
        snprintf(response, sizeof(response),
                 "{\"ok\":false,\"error\":\"%s\",\"state_only\":true}", esp_err_to_name(ret));
        return httpd_resp_sendstr(req, response);
    }
    return mode_status_response(req);
}

esp_err_t mode_http_api_register(void)
{
    const httpd_uri_t get_uri = {
        .uri = "/api/mode", .method = HTTP_GET, .handler = mode_status_response,
    };
    const httpd_uri_t post_uri = {
        .uri = "/api/mode", .method = HTTP_POST, .handler = mode_post_handler,
    };
    esp_err_t ret = ws_manager_server_register_uri(&get_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Register GET /api/mode failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = ws_manager_server_register_uri(&post_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Register POST /api/mode failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "GET/POST /api/mode registered (state/countdown only)");
    return ESP_OK;
}
