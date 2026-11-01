/* Actual mode_http_api.c + controller_modes.c + cJSON. No network or board.
 * Calls only registered route handlers; request bodies arrive in small reads. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_http_server.h"
#include "controller_modes.h"
#include "../../controller/main/mode_http_api.h"
#include "../../controller/main/demo_behaviors.h"
#include "task_guide.h"

int64_t fake_now;
static httpd_uri_t routes[4];
static unsigned route_count;
static size_t largest_compact, full_size;
static esp_err_t guide_report_result=ESP_OK;
static unsigned guide_reports;

cJSON *task_guide_snapshot(void)
{
    return cJSON_Parse("{\"controller_boot_id\":4294967295,\"request_id\":3,\"state\":\"ready\",\"steps\":[\"丢掉桌面垃圾\",\"把物品分类放好\",\"擦净桌面\"]}");
}
esp_err_t task_guide_report(const cJSON *report)
{
    ++guide_reports;
    if(!cJSON_IsObject(report))return ESP_ERR_INVALID_ARG;
    return guide_report_result;
}

esp_err_t ws_manager_server_register_uri(const httpd_uri_t *uri)
{
    assert(route_count < 4); routes[route_count++]=*uri; return ESP_OK;
}
void demo_behaviors_get_status(demo_behaviors_status_t *out)
{
    *out=(demo_behaviors_status_t){.started=true,.head_sync_ready=true,.radar_fresh=true};
}
int httpd_req_recv(httpd_req_t *req,char *buffer,size_t length)
{
    if(req->consumed==req->input_size)return 0;
    size_t size=req->input_size-req->consumed;
    if(size>length)size=length;
    if(req->receive_chunk && size>req->receive_chunk)size=req->receive_chunk;
    memcpy(buffer,req->input+req->consumed,size);req->consumed+=size;return (int)size;
}
esp_err_t httpd_resp_set_type(httpd_req_t *req,const char *type)
{ snprintf(req->content_type,sizeof(req->content_type),"%s",type);return ESP_OK; }
esp_err_t httpd_resp_set_status(httpd_req_t *req,const char *status)
{ snprintf(req->status,sizeof(req->status),"%s",status);return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *req,const char *name,const char *value)
{ if(!strcmp(name,"Connection") && !strcmp(value,"close"))req->close_header=1;return ESP_OK; }
esp_err_t httpd_resp_send(httpd_req_t *req,const char *body,int length)
{
    assert(length>=0 && (size_t)length<sizeof(req->response));
    memcpy(req->response,body,(size_t)length);req->response[length]=0;
    req->response_size=(size_t)length;return ESP_OK;
}
esp_err_t httpd_resp_sendstr(httpd_req_t *req,const char *body)
{ return httpd_resp_send(req,body,(int)strlen(body)); }
esp_err_t httpd_resp_send_chunk(httpd_req_t *req,const char *body,int length)
{
    assert(length>=0 && req->response_size+(size_t)length<sizeof(req->response));
    if(length)memcpy(req->response+req->response_size,body,(size_t)length);
    req->response_size+=(size_t)length;req->response[req->response_size]=0;
    return ESP_OK;
}
esp_err_t httpd_resp_sendstr_chunk(httpd_req_t *req,const char *body)
{ return httpd_resp_send_chunk(req,body,(int)strlen(body)); }
esp_err_t httpd_resp_send_err(httpd_req_t *req,int code,const char *message)
{ (void)message;snprintf(req->status,sizeof(req->status),"%d",code);return httpd_resp_sendstr(req,"{}"); }

static httpd_req_t call_raw(int method,const char *path,const char *body,size_t bytes,size_t declared)
{
    httpd_req_t request={.content_len=declared,.input=body,.input_size=bytes,.receive_chunk=7};
    for(unsigned i=0;i<route_count;i++)if(routes[i].method==method && !strcmp(routes[i].uri,path)) {
        (void)routes[i].handler(&request);
        assert(request.response_size && !strcmp(request.content_type,"application/json"));
        assert(request.response_size==strlen(request.response));return request;
    }
    assert(!"route not registered");return request;
}
static httpd_req_t call(int method,const char *path,const char *body)
{ size_t size=body?strlen(body):0;return call_raw(method,path,body,size,size); }
static int status(const httpd_req_t *r) { return *r->status?atoi(r->status):200; }
static cJSON *json(const httpd_req_t *r)
{
    cJSON *root=cJSON_ParseWithOpts(r->response,NULL,true);
    assert(cJSON_IsObject(root));return root;
}
static uint32_t u32(const cJSON *root,const char *name)
{
    cJSON *item=cJSON_GetObjectItemCaseSensitive(root,name);
    assert(cJSON_IsNumber(item) && item->valuedouble>=0 && item->valuedouble<=UINT32_MAX);
    return (uint32_t)item->valuedouble;
}
static void authority(const cJSON *root)
{
    const char *numbers[]={"uptime_ms","controller_boot_id","session_id","seq","engagement_token",
        "duration_ms","remaining_ms","rate_half"};
    for(unsigned i=0;i<sizeof(numbers)/sizeof(numbers[0]);i++)(void)u32(root,numbers[i]);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root,"ok")));
    assert(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root,"mode")));
    assert(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root,"pause_reason")));
    assert(cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(root,"paused")));
    assert(cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(root,"completed")));
}
static cJSON *compact(const httpd_req_t *r)
{
    assert(status(r)==200 && r->response_size<512);
    if(r->response_size>largest_compact)largest_compact=r->response_size;
    cJSON *root=json(r);authority(root);
    assert(cJSON_GetObjectItemCaseSensitive(root,"http_stack_free_bytes")==NULL);
    assert(cJSON_GetObjectItemCaseSensitive(root,"radar_present")==NULL);
    return root;
}
static void bad(const char *body)
{
    httpd_req_t r=call(HTTP_POST,"/api/mode/engagement",body);
    assert(status(&r)==400);cJSON *j=json(&r);
    assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(j,"ok")));cJSON_Delete(j);
}
int main(void)
{
    assert(controller_modes_init()==ESP_OK);
    assert(mode_http_api_register()==ESP_OK && route_count==4);
    /* Longest uptime and boot/token identities; max configurable duration. */
    fake_now=(int64_t)UINT32_MAX*1000;
    httpd_req_t r=call(HTTP_POST,"/api/mode","{\"mode\":\"focus\",\"duration\":180}");
    assert(status(&r)==200);
    cJSON *j=json(&r);authority(j);
    uint32_t boot=u32(j,"controller_boot_id"),session=u32(j,"session_id");
    assert(boot==UINT32_MAX && u32(j,"remaining_ms")==10800000);cJSON_Delete(j);
    char body[512];
    snprintf(body,sizeof(body),"{\"bind\":true,\"controller_boot_id\":%lu,\"session_id\":%lu}",
        (unsigned long)boot,(unsigned long)session);
    r=call(HTTP_POST,"/api/mode/engagement",body);j=compact(&r);
    uint32_t token=u32(j,"engagement_token");assert(token==UINT32_MAX-1);cJSON_Delete(j);
    for(unsigned seq=1;seq<=3;seq++) {
        fake_now+=1000000;
        snprintf(body,sizeof(body),"{\"observed_ms\":%lu,\"controller_boot_id\":%lu,\"session_id\":%lu,"
            "\"engagement_token\":%lu,\"sample_seq\":%u,\"inference_seq\":%u,\"age_ms\":0,"
            "\"sample_age_ms\":0,\"valid\":true,\"level\":\"Engaged\"}",
            (unsigned long)(uint32_t)(fake_now/1000),(unsigned long)boot,(unsigned long)session,
            (unsigned long)token,seq,seq);
        r=call(HTTP_POST,"/api/mode/engagement",body);j=compact(&r);
        assert(u32(j,"session_id")==session);
        if(seq>=2)assert(u32(j,"rate_half")==2);
        cJSON_Delete(j);
    }
    /* The sender has held a clock echo for 5 s. Only the 100 ms network
     * remainder counts as transport; legacy sender (no hold) would reject it. */
    fake_now+=1000000;
    snprintf(body,sizeof(body),"{\"observed_ms\":%lu,\"observed_hold_ms\":5000,\"controller_boot_id\":%lu,\"session_id\":%lu,"
        "\"engagement_token\":%lu,\"sample_seq\":4,\"inference_seq\":4,\"age_ms\":0,"
        "\"sample_age_ms\":0,\"valid\":true,\"level\":\"Engaged\"}",
        (unsigned long)((uint32_t)(fake_now/1000)-5100),(unsigned long)boot,(unsigned long)session,(unsigned long)token);
    r=call(HTTP_POST,"/api/mode/engagement",body);j=compact(&r);
    assert(u32(j,"rate_half")==2);cJSON_Delete(j);
    /* A larger sender hold than receiver elapsed must never clamp into freshness. */
    snprintf(body,sizeof(body),"{\"observed_ms\":%lu,\"observed_hold_ms\":101,\"controller_boot_id\":%lu,\"session_id\":%lu,"
        "\"engagement_token\":%lu,\"sample_seq\":5,\"inference_seq\":5,\"age_ms\":0,"
        "\"sample_age_ms\":0,\"valid\":true,\"level\":\"Engaged\"}",
        (unsigned long)((uint32_t)(fake_now/1000)-100),(unsigned long)boot,(unsigned long)session,(unsigned long)token);
    r=call(HTTP_POST,"/api/mode/engagement",body);assert(status(&r)==400);
    snprintf(body,sizeof(body),"{\"observed_ms\":%lu,\"observed_hold_ms\":-1,\"controller_boot_id\":%lu,\"session_id\":%lu,"
        "\"engagement_token\":%lu,\"sample_seq\":5,\"inference_seq\":5,\"age_ms\":0,"
        "\"sample_age_ms\":0,\"valid\":true,\"level\":\"Engaged\"}",
        (unsigned long)(uint32_t)(fake_now/1000),(unsigned long)boot,(unsigned long)session,(unsigned long)token);
    r=call(HTTP_POST,"/api/mode/engagement",body);assert(status(&r)==400);
    r=call(HTTP_GET,"/api/mode",NULL);assert(status(&r)==200);full_size=r.response_size;
    j=json(&r);authority(j);
    assert(u32(j,"http_stack_free_bytes")==6144);
    cJSON *totals=cJSON_GetObjectItemCaseSensitive(j,"timing_totals");
    assert(cJSON_IsObject(totals));
    assert(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(totals,"full_ms")));
    assert(u32(totals,"held_ms")==u32(totals,"waiting_ms")+u32(totals,"distracted_ms")+
        u32(totals,"invalid_ms")+u32(totals,"data_stale_ms")+u32(totals,"voice_ms"));
    assert(cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(j,"radar_present")));
    assert(cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(j,"demo_worker")));
    cJSON *guide=cJSON_GetObjectItemCaseSensitive(j,"task_guide");
    assert(cJSON_IsObject(guide));
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(guide,"state")->valuestring,"ready"));
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(guide,"steps"))==3);
    assert(!strcmp(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(guide,"steps"),0)->valuestring,"丢掉桌面垃圾"));
    assert(full_size>largest_compact);cJSON_Delete(j);
    /* An old session must reject instead of restarting its progress. */
    snprintf(body,sizeof(body),"{\"bind\":true,\"controller_boot_id\":%lu,\"session_id\":%lu}",
        (unsigned long)boot,(unsigned long)(session-1));
    r=call(HTTP_POST,"/api/mode/engagement",body);assert(status(&r)==409);j=json(&r);
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(j,"error")->valuestring,"resync_required"));cJSON_Delete(j);
    bad("{");bad("{}");bad("{\"bind\":true,\"controller_boot_id\":0.5,\"session_id\":1}");
    r=call_raw(HTTP_POST,"/api/mode/engagement","{",1,20);assert(status(&r)==400 && r.close_header);
    char oversized[512];memset(oversized,'x',sizeof(oversized));
    r=call_raw(HTTP_POST,"/api/mode/engagement",oversized,sizeof(oversized),sizeof(oversized));
    assert(status(&r)==400 && r.close_header);
    static const char nul_body[]="{\"bind\":true}\0junk";
    r=call_raw(HTTP_POST,"/api/mode/engagement",nul_body,sizeof(nul_body)-1,sizeof(nul_body)-1);
    assert(status(&r)==400);

    /* Evidence must remain available after rejected requests, without
     * changing authority or enlarging the compact engagement response. */
    r=call(HTTP_GET,"/api/mode",NULL);assert(status(&r)==200);
    j=json(&r);
    cJSON *diagnostics=cJSON_GetObjectItemCaseSensitive(j,"engagement_diagnostics");
    assert(cJSON_IsObject(diagnostics));
    assert(u32(diagnostics,"version")==1);
    assert(u32(diagnostics,"accepted")>0);
    assert(u32(diagnostics,"clock_rejected")>0);
    assert(u32(diagnostics,"conflict")>0);
    assert(u32(diagnostics,"malformed")>0);
    assert(u32(diagnostics,"received")>=u32(diagnostics,"accepted"));
    cJSON_Delete(j);

    /* Task guide route framing and status map. Component data is a fixture;
     * request gate/late-report semantics are exercised in task_guide_gate_test.c. */
    r=call(HTTP_POST,"/api/mode/task_guide","{\"request_id\":3}");
    assert(status(&r)==200 && guide_reports==1);
    j=json(&r);assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j,"ok")));cJSON_Delete(j);
    guide_report_result=ESP_ERR_INVALID_STATE;
    r=call(HTTP_POST,"/api/mode/task_guide","{}");assert(status(&r)==409);
    guide_report_result=ESP_ERR_INVALID_ARG;
    r=call(HTTP_POST,"/api/mode/task_guide","{}");assert(status(&r)==400);
    r=call_raw(HTTP_POST,"/api/mode/task_guide","{",1,20);assert(status(&r)==400 && r.close_header);
    r=call_raw(HTTP_POST,"/api/mode/task_guide",oversized,sizeof(oversized),sizeof(oversized));
    assert(status(&r)==400 && r.close_header);
    r=call_raw(HTTP_POST,"/api/mode/task_guide",nul_body,sizeof(nul_body)-1,sizeof(nul_body)-1);
    assert(status(&r)==400);
    printf("PASS: actual HTTP handlers + controller_modes + cJSON; compact max=%zu/512, full=%zu/2048; "
           "bind/sample authority, residence subtraction, invalid hold400, GET diagnostics, uint32 wrap, stale session409, malformed/truncated/oversized/NUL400\n",
           largest_compact,full_size);
}
