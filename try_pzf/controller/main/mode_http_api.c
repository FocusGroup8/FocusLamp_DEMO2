#include "mode_http_api.h"

#include <errno.h>
#include <math.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "cJSON.h"
#include "controller_modes.h"
#include "demo_behaviors.h"
#include "task_guide.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "websocket_manager.h"
#include "../../shared/focus_transport_age.h"

static const char *TAG = "MODE_API";
static esp_err_t engagement_response(httpd_req_t *req);

/* Reapply for each normal response: IDF error helpers may clear this option
 * on a persistent socket. Failure must not suppress the existing response. */
static void nodelay_on(httpd_req_t *req, const char *site)
{
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) {
        ESP_LOGW(TAG, "%s nodelay skipped: no socket", site);
        return;
    }
    int one = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0) {
        ESP_LOGW(TAG, "%s nodelay failed: errno=%d", site, errno);
    }
}

/* HTTPD serializes these handlers. Small retained evidence, no per-frame log I/O. */
static struct {
    uint32_t received, accepted, malformed, clock_rejected, conflict;
    uint32_t sample_seq, inference_seq, age_ms, sample_age_ms, transit_ms;
    uint32_t echo_elapsed_ms, echo_hold_ms;
    int result;
} engagement_diag;
static cJSON *engagement_diagnostics(void)
{
    cJSON *j=cJSON_CreateObject();if(!j)return NULL;
    cJSON_AddNumberToObject(j,"version",1);
#define ADD(field) cJSON_AddNumberToObject(j,#field,engagement_diag.field)
    ADD(received);ADD(accepted);ADD(malformed);ADD(clock_rejected);ADD(conflict);
    ADD(sample_seq);ADD(inference_seq);ADD(age_ms);ADD(sample_age_ms);ADD(transit_ms);ADD(result);
    ADD(echo_elapsed_ms);ADD(echo_hold_ms);
#undef ADD
    return j;
}

/* Reports are copied under the modes lock; all JSON allocation happens here. */
static char *focus_reports_json(void)
{
    controller_focus_reports_t *reports=malloc(sizeof(*reports));
    if(!reports)return NULL;
    if(controller_modes_get_focus_reports(reports)!=ESP_OK){free(reports);return NULL;}
    cJSON *root=cJSON_CreateObject(),*pending=NULL;
    if(!root)goto fail;
    if(!cJSON_AddNumberToObject(root,"schema_version",1) ||
       !cJSON_AddNumberToObject(root,"dropped_reports",reports->dropped_reports))goto fail;
    pending=cJSON_AddArrayToObject(root,"pending");
    if(!pending)goto fail;
    for(uint32_t i=0;i<reports->count;++i){
        const controller_focus_report_t *r=&reports->pending[i];
        cJSON *j=cJSON_CreateObject();
        if(!j)goto fail;
        if(!cJSON_AddItemToArray(pending,j)){cJSON_Delete(j);goto fail;}
#define ADD_REPORT(field) if(!cJSON_AddNumberToObject(j,#field,(double)r->field))goto fail
        if(!cJSON_AddNumberToObject(j,"schema_version",1))goto fail;
        ADD_REPORT(controller_boot_id);ADD_REPORT(session_id);ADD_REPORT(duration_ms);ADD_REPORT(remaining_ms);
        ADD_REPORT(started_uptime_ms);ADD_REPORT(ended_uptime_ms);ADD_REPORT(effective_ms);ADD_REPORT(elapsed_ms);
        if(!cJSON_AddBoolToObject(j,"completed",r->completed) ||
           !cJSON_AddStringToObject(j,"end_reason",controller_modes_focus_end_name(r->end_reason)))goto fail;
        cJSON *totals=cJSON_AddObjectToObject(j,"timing_totals");
        if(!totals)goto fail;
#undef ADD_REPORT
#define ADD_TOTAL(field) if(!cJSON_AddNumberToObject(totals,#field,(double)r->field))goto fail
        ADD_TOTAL(full_ms);ADD_TOTAL(normal_ms);ADD_TOTAL(held_ms);ADD_TOTAL(waiting_ms);
        ADD_TOTAL(distracted_ms);ADD_TOTAL(invalid_ms);ADD_TOTAL(data_stale_ms);ADD_TOTAL(voice_ms);
#undef ADD_TOTAL
    }
    char *result=cJSON_PrintUnformatted(root);
    cJSON_Delete(root);free(reports);
    return result;
fail:
    cJSON_Delete(root);free(reports);return NULL;
}

static esp_err_t mode_status_response(httpd_req_t *req)
{
#ifdef FOCUS_SAMPLED_DEMO
    /* The demo head needs the clock, not a multi-chunk diagnostics snapshot. */
    const char *query = strchr(req->uri, '?');
    if (query && strcmp(query, "?compact=1") == 0) {
        httpd_resp_set_type(req, "application/json");
        return engagement_response(req);
    }
#endif
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
    char response[1200];
    snprintf(response, sizeof(response),
        "{\"http_stack_free_bytes\":%u,\"uptime_ms\":%lu,\"duration_ms\":%lu,\"remaining_ms\":%lu,\"controller_boot_id\":%lu,"
        "\"session_id\":%lu,\"seq\":%lu,\"revision\":%lu,\"engagement_token\":%lu,"
        "\"rate_quarters\":%d,\"rate\":%g,\"rate_half\":%g,\"pause_reason\":\"%s\","
        "\"ok\":true,\"mode\":\"%s\",\"app_state\":%u,"
        "\"duration_sec\":%lu,\"remaining_sec\":%lu,\"voice_active\":%s,"
        "\"paused\":%s,\"completed\":%s,\"state_only\":%s,"
        "\"demo_worker\":%s,\"companion_local_ready\":%s,\"head_sync_ready\":%s,"
        "\"radar_fresh\":%s,\"radar_present\":%s,\"sedentary_sec\":%u,\"sedentary_pending\":%s,"
        "\"sedentary_armed\":%s,\"sedentary_submitted\":%s,\"sedentary_text_observed\":%s,"
        "\"sedentary_local_output_done\":%s,\"sedentary_cloud_tts_done\":%s,\"sedentary_request_id\":%lu}",
        (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned long)state.uptime_ms, (unsigned long)state.duration_ms, (unsigned long)state.remaining_ms,
        (unsigned long)state.controller_boot_id, (unsigned long)state.session_id,
        (unsigned long)state.seq, (unsigned long)state.revision, (unsigned long)state.engagement_token,
        state.rate_quarters, state.rate_quarters / 4.0, state.rate_half, controller_modes_pause_name(state.pause_reason),
        controller_modes_name(state.mode), (unsigned)state.mode,
        (unsigned long)state.duration_sec, (unsigned long)state.remaining_sec,
        state.voice_active ? "true" : "false", state.paused ? "true" : "false",
        state.completed ? "true" : "false",
        state.mode == APP_STATE_COMPANION && demo.started ? "false" : "true",
        demo.started ? "true" : "false", same && demo.companion_local_ready ? "true" : "false",
        same && demo.head_sync_ready ? "true" : "false", same && demo.radar_fresh ? "true" : "false",
        same && demo.radar_present ? "true" : "false", same ? (unsigned)demo.sedentary_sec : 0U,
        same && demo.sedentary_pending ? "true" : "false",
        same && demo.sedentary_armed ? "true" : "false",
        same && demo.sedentary_submitted ? "true" : "false",
        same && demo.sedentary_text_observed ? "true" : "false",
        same && demo.sedentary_local_output_done ? "true" : "false",
        same && demo.sedentary_cloud_tts_done ? "true" : "false",
        same ? (unsigned long)demo.sedentary_request_id : 0UL);
    cJSON *guide=task_guide_snapshot();
    char *guide_json=guide?cJSON_PrintUnformatted(guide):NULL;
    cJSON_Delete(guide);
    if (!guide_json) return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"guide snapshot unavailable");
    char *reports_json=focus_reports_json();
    if(!reports_json){
        free(guide_json);
        return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"focus reports unavailable");
    }
    /* Stream the extension without doubling the HTTP task's stack footprint. */
    size_t length=strlen(response);
    nodelay_on(req, "mode_status_response");
    esp_err_t sent=httpd_resp_send_chunk(req,response,length-1);
    char accounting[384];
    snprintf(accounting,sizeof(accounting),
#ifdef FOCUS_SAMPLED_DEMO
        ",\"sampled_demo\":true,\"sample_ttl_ms\":6000"
#else
        ",\"sampled_demo\":false,\"sample_ttl_ms\":3000"
#endif
        ",\"timing_totals\":{\"full_ms\":%lu,\"normal_ms\":%lu,\"half_ms\":%lu,\"held_ms\":%lu,"
        "\"waiting_ms\":%lu,\"distracted_ms\":%lu,\"invalid_ms\":%lu,\"data_stale_ms\":%lu,\"voice_ms\":%lu}",
        (unsigned long)state.full_ms,(unsigned long)state.normal_ms,(unsigned long)state.half_ms,(unsigned long)state.held_ms,
        (unsigned long)state.held_waiting_ms,(unsigned long)state.held_distracted_ms,
        (unsigned long)state.held_invalid_ms,(unsigned long)state.held_stale_ms,(unsigned long)state.held_voice_ms);
    if(sent==ESP_OK)sent=httpd_resp_sendstr_chunk(req,accounting);
    if(sent==ESP_OK)sent=httpd_resp_sendstr_chunk(req,",\"task_guide\":");
    if(sent==ESP_OK)sent=httpd_resp_sendstr_chunk(req,guide_json);
    free(guide_json);
    if(sent==ESP_OK)sent=httpd_resp_sendstr_chunk(req,",\"focus_reports\":");
    if(sent==ESP_OK)sent=httpd_resp_sendstr_chunk(req,reports_json);
    free(reports_json);
    cJSON *diagnostics=engagement_diagnostics();
    char *diag_json=diagnostics?cJSON_PrintUnformatted(diagnostics):NULL;cJSON_Delete(diagnostics);
    if(sent==ESP_OK && diag_json)sent=httpd_resp_sendstr_chunk(req,",\"engagement_diagnostics\":");
    if(sent==ESP_OK && diag_json)sent=httpd_resp_sendstr_chunk(req,diag_json);
    free(diag_json);
    if(sent==ESP_OK)sent=httpd_resp_sendstr_chunk(req,"}");
    if(sent==ESP_OK)sent=httpd_resp_send_chunk(req,NULL,0);
    return sent;
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

static bool json_u32(const cJSON *root, const char *key, uint32_t *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < 0 ||
        v->valuedouble > UINT32_MAX || floor(v->valuedouble) != v->valuedouble) return false;
    *out = (uint32_t)v->valuedouble; return true;
}

static esp_err_t focus_report_ack_handler(httpd_req_t *req)
{
    char body[193];size_t got=0;
    httpd_resp_set_type(req,"application/json");
    if(!req->content_len || req->content_len>=sizeof(body))goto bad;
    while(got<req->content_len){
        int n=httpd_req_recv(req,body+got,req->content_len-got);
        if(n<=0)goto bad;
        got+=(size_t)n;
    }
    body[got]=0;
    if(memchr(body,0,got))goto bad;
    cJSON *root=cJSON_ParseWithOpts(body,NULL,true);
    uint32_t boot=0,session=0;
    bool valid=cJSON_IsObject(root) && json_u32(root,"controller_boot_id",&boot) &&
        json_u32(root,"session_id",&session) && session>0;
    cJSON_Delete(root);
    if(!valid)goto bad;
    if(controller_modes_ack_focus_report(boot,session)!=ESP_OK){
        httpd_resp_set_status(req,"409 Conflict");
        return httpd_resp_sendstr(req,"{\"ok\":false,\"error\":\"controller_boot_changed\"}");
    }
    nodelay_on(req,"focus_report_ack");
    return httpd_resp_sendstr(req,"{\"ok\":true}");
bad:
    httpd_resp_set_status(req,"400 Bad Request");
    httpd_resp_set_hdr(req,"Connection","close");
    httpd_resp_sendstr(req,"{\"ok\":false,\"error\":\"invalid focus report acknowledgement\"}");
    return ESP_FAIL;
}
/* Compact reply keeps the clock and task guide consumed by the head,
 * while omitting the full diagnostic payload. */
static esp_err_t engagement_response(httpd_req_t *req)
{
    controller_modes_status_t s;
    if(controller_modes_get_status(&s)!=ESP_OK) {
        httpd_resp_set_status(req,"503 Service Unavailable");
        return httpd_resp_sendstr(req,"{\"ok\":false,\"error\":\"modes not ready\"}");
    }
    char body[512];
    int n=snprintf(body,sizeof(body),
        "{\"ok\":true,\"mode\":\"%s\",\"uptime_ms\":%lu,\"controller_boot_id\":%lu,"
        "\"session_id\":%lu,\"seq\":%lu,\"engagement_token\":%lu,\"duration_ms\":%lu,"
        "\"remaining_ms\":%lu,\"rate_quarters\":%d,\"rate\":%g,\"rate_half\":%g,\"pause_reason\":\"%s\",\"paused\":%s,\"completed\":%s}",
        controller_modes_name(s.mode),(unsigned long)s.uptime_ms,(unsigned long)s.controller_boot_id,
        (unsigned long)s.session_id,(unsigned long)s.seq,(unsigned long)s.engagement_token,
        (unsigned long)s.duration_ms,(unsigned long)s.remaining_ms,s.rate_quarters,s.rate_quarters / 4.0,s.rate_half,
        controller_modes_pause_name(s.pause_reason),s.paused?"true":"false",s.completed?"true":"false");
    if(n<0 || n>=(int)sizeof(body))return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"response overflow");
    static int64_t report_at;
    int64_t now=esp_timer_get_time();
    if(now-report_at>=10000000){
        report_at=now;
        ESP_LOGI(TAG,"[MODE_HTTP] stack_free_bytes=%u session=%lu reason=%s",(unsigned)uxTaskGetStackHighWaterMark(NULL),
            (unsigned long)s.session_id,controller_modes_pause_name(s.pause_reason));
    }
    nodelay_on(req, "engagement_response");
    if(!strchr(req->uri,'?'))return httpd_resp_send(req,body,n);
    cJSON *reply=cJSON_Parse(body);
    cJSON *guide=task_guide_snapshot();
    if(!reply || !guide) {
        cJSON_Delete(reply);cJSON_Delete(guide);
        return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"guide snapshot unavailable");
    }
    cJSON_AddItemToObject(reply,"task_guide",guide);
    char *json=cJSON_PrintUnformatted(reply);cJSON_Delete(reply);
    if(!json)return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"response unavailable");
    esp_err_t result=httpd_resp_sendstr(req,json);
    free(json);
    return result;
}
static esp_err_t engagement_handler(httpd_req_t *req)
{
    ++engagement_diag.received;engagement_diag.result=400;
    char body[512]; size_t got = 0;
    httpd_resp_set_type(req, "application/json");
    if (!req->content_len || req->content_len >= sizeof(body)) goto bad;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) goto bad;
        got += n;
    }
    body[got] = 0;
    if (memchr(body, 0, got)) goto bad;
    cJSON *root = cJSON_ParseWithOpts(body, NULL, true);
    uint32_t boot, session, observed=0, hold=0, token=0, sample=0, inference=0, age=0, sample_age=0;
    bool bind = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root,"bind"));
    const cJSON *level=cJSON_GetObjectItemCaseSensitive(root,"level");
    const cJSON *valid=cJSON_GetObjectItemCaseSensitive(root,"valid");
    bool ok = json_u32(root,"controller_boot_id",&boot) && json_u32(root,"session_id",&session);
    if (!bind) ok = ok && json_u32(root,"observed_ms",&observed) && json_u32(root,"engagement_token",&token) &&
        json_u32(root,"sample_seq",&sample) && json_u32(root,"inference_seq",&inference) &&
        json_u32(root,"age_ms",&age) && json_u32(root,"sample_age_ms",&sample_age) &&
        cJSON_IsString(level) && cJSON_IsBool(valid);
    if (ok && !bind) {
        engagement_diag.sample_seq=sample;engagement_diag.inference_seq=inference;
        uint32_t transit;
        const cJSON *hold_item=cJSON_GetObjectItemCaseSensitive(root,"observed_hold_ms");
        engagement_diag.echo_elapsed_ms=(uint32_t)(esp_timer_get_time()/1000)-observed;
        engagement_diag.echo_hold_ms=0;
        (void)json_u32(root,"observed_hold_ms",&engagement_diag.echo_hold_ms);
        if ((hold_item && !json_u32(root,"observed_hold_ms",&hold)) ||
            !focus_transport_age((uint32_t)(esp_timer_get_time()/1000),observed,hold,&transit)) {
            ok=false;++engagement_diag.clock_rejected;
        }
        else {
            engagement_diag.transit_ms=transit;
            age=focus_age_add(age,transit);
            sample_age=focus_age_add(sample_age,transit);
        }
        engagement_diag.age_ms=age;engagement_diag.sample_age_ms=sample_age;
    }
    esp_err_t ret = ok ? controller_modes_engagement(boot,session,token,bind,sample,inference,
        age,sample_age,cJSON_IsTrue(valid),cJSON_IsString(level)?level->valuestring:"") : ESP_ERR_INVALID_ARG;
    cJSON_Delete(root);
    if (!ok) goto bad;
    if (ret != ESP_OK) {
        ++engagement_diag.conflict;engagement_diag.result=409;
        httpd_resp_set_status(req,"409 Conflict");
        return httpd_resp_sendstr(req,"{\"ok\":false,\"error\":\"resync_required\"}");
    }
    ++engagement_diag.accepted;engagement_diag.result=200;
    return engagement_response(req);
bad:
    ++engagement_diag.malformed;
    httpd_resp_set_status(req,"400 Bad Request");
    httpd_resp_set_hdr(req,"Connection","close");
    httpd_resp_sendstr(req,"{\"ok\":false,\"error\":\"invalid engagement\"}");
    return ESP_FAIL;
}

static esp_err_t task_guide_handler(httpd_req_t *req)
{
    char body[384];size_t got=0;
    httpd_resp_set_type(req,"application/json");
    if(!req->content_len||req->content_len>=sizeof(body))goto bad;
    while(got<req->content_len){
        int n=httpd_req_recv(req,body+got,req->content_len-got);
        if(n<=0)goto bad;
        got+=(size_t)n;
    }
    body[got]=0;
    if(memchr(body,0,got))goto bad;
    cJSON *j=cJSON_ParseWithOpts(body,NULL,true);
    esp_err_t ret=task_guide_report(j);cJSON_Delete(j);
    if(ret==ESP_ERR_INVALID_ARG)goto bad;
    if(ret!=ESP_OK){
        httpd_resp_set_status(req,"409 Conflict");
        return httpd_resp_sendstr(req,"{\"ok\":false,\"error\":\"stale guide report\"}");
    }
    return httpd_resp_sendstr(req,"{\"ok\":true}");
bad:
    httpd_resp_set_status(req,"400 Bad Request");
    httpd_resp_set_hdr(req,"Connection","close");
    httpd_resp_sendstr(req,"{\"ok\":false,\"error\":\"invalid guide report\"}");
    return ESP_FAIL;
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
    const httpd_uri_t engagement_uri = {.uri="/api/mode/engagement", .method=HTTP_POST, .handler=engagement_handler};
    ret = ws_manager_server_register_uri(&engagement_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Register POST /api/mode/engagement failed: %s", esp_err_to_name(ret));
        return ret;
    }
    const httpd_uri_t guide_uri={.uri="/api/mode/task_guide",.method=HTTP_POST,.handler=task_guide_handler};
    ret=ws_manager_server_register_uri(&guide_uri);
    if(ret!=ESP_OK)return ret;
    const httpd_uri_t report_ack_uri={.uri="/api/focus/reports/ack",.method=HTTP_POST,.handler=focus_report_ack_handler};
    ret=ws_manager_server_register_uri(&report_ack_uri);
    if(ret!=ESP_OK)return ret;
    ESP_LOGI(TAG, "Mode and engagement APIs registered");
    return ESP_OK;
}
