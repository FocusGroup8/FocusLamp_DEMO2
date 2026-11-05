/* Actual mode_http_api.c + controller_modes.c + cJSON. No network or board.
 * Calls only registered route handlers; request bodies arrive in small reads. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include "../../head/main/algo/focus_rate_protocol.h"
#include <errno.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include "cJSON.h"
#include "esp_http_server.h"
#include "controller_modes.h"
#include "../../controller/main/mode_http_api.h"
#include "../../controller/main/demo_behaviors.h"
#include "task_guide.h"

int64_t fake_now;
static httpd_uri_t routes[5];
static unsigned route_count;
static size_t largest_compact, full_size;
static esp_err_t guide_report_result=ESP_OK;
static unsigned guide_reports;
static int request_fd=17, socket_nodelay, fail_sockopt, fail_guide_snapshot;
static unsigned sockopt_calls;

int httpd_req_to_sockfd(httpd_req_t *req) { return req->fd; }
int setsockopt(int fd,int level,int name,const void *value,socklen_t length)
{
    assert(fd==17 && level==IPPROTO_TCP && name==TCP_NODELAY);
    assert(length==sizeof(int) && value!=NULL);
    ++sockopt_calls;
    if(fail_sockopt){errno=EINVAL;return -1;}
    socket_nodelay=*(const int *)value;
    return 0;
}
static void record_send(httpd_req_t *req)
{
    if(req->send_calls++==0)req->nodelay_at_first_send=socket_nodelay;
}

cJSON *task_guide_snapshot(void)
{
    if(fail_guide_snapshot)return NULL;
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
    assert(route_count < 5); routes[route_count++]=*uri; return ESP_OK;
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
    record_send(req);
    assert(length>=0 && (size_t)length<sizeof(req->response));
    memcpy(req->response,body,(size_t)length);req->response[length]=0;
    req->response_size=(size_t)length;return ESP_OK;
}
esp_err_t httpd_resp_sendstr(httpd_req_t *req,const char *body)
{ return httpd_resp_send(req,body,(int)strlen(body)); }
esp_err_t httpd_resp_send_chunk(httpd_req_t *req,const char *body,int length)
{
    record_send(req);
    assert(length>=0 && req->response_size+(size_t)length<sizeof(req->response));
    if(length)memcpy(req->response+req->response_size,body,(size_t)length);
    req->response_size+=(size_t)length;req->response[req->response_size]=0;
    return ESP_OK;
}
esp_err_t httpd_resp_sendstr_chunk(httpd_req_t *req,const char *body)
{ return httpd_resp_send_chunk(req,body,(int)strlen(body)); }
esp_err_t httpd_resp_send_err(httpd_req_t *req,int code,const char *message)
{
    /* IDF 5.5.4 error-helper behavior: successful set-to-1 is followed by
     * set-to-0, even if the persistent socket was already using NODELAY. */
    (void)message;
    int one=1, zero=0;
    assert(setsockopt(req->fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one))==0);
    snprintf(req->status,sizeof(req->status),"%d",code);
    esp_err_t result=httpd_resp_sendstr(req,"{}");
    assert(setsockopt(req->fd,IPPROTO_TCP,TCP_NODELAY,&zero,sizeof(zero))==0);
    return result;
}

static httpd_req_t call_raw(int method,const char *path,const char *body,size_t bytes,size_t declared)
{
    httpd_req_t request={.uri=path,.content_len=declared,.input=body,.input_size=bytes,.receive_chunk=7,.fd=request_fd};
    size_t path_length=strcspn(path,"?");
    for(unsigned i=0;i<route_count;i++)if(routes[i].method==method && strlen(routes[i].uri)==path_length && !strncmp(routes[i].uri,path,path_length)) {
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
        "duration_ms","remaining_ms","rate_quarters"};
    for(unsigned i=0;i<sizeof(numbers)/sizeof(numbers[0]);i++)(void)u32(root,numbers[i]);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root,"ok")));
    assert(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root,"mode")));
    assert(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root,"pause_reason")));
    assert(cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(root,"paused")));
    assert(cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(root,"completed")));
    int quarters=-1;assert(focus_rate_quarters(root,&quarters));
    assert(cJSON_GetObjectItemCaseSensitive(root,"rate_half")->valuedouble==quarters/2.0);
    assert(cJSON_GetObjectItemCaseSensitive(root,"rate")->valuedouble==quarters/4.0);
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
static void nodelay_contract(const char *bind_body)
{
    const struct {int method;const char *path,*body;} cases[]={
        {HTTP_GET,"/api/mode",NULL},
        {HTTP_POST,"/api/mode/engagement",bind_body},
    };
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        unsigned before=sockopt_calls;
        socket_nodelay=0;
        httpd_req_t r=call(cases[i].method,cases[i].path,cases[i].body);
        assert(status(&r)==200 && sockopt_calls==before+1 && r.nodelay_at_first_send==1);
        cJSON *j=json(&r);authority(j);cJSON_Delete(j);

        /* Reapply even when already enabled: a reused connection must not
         * rely on open_fn, nor on the previous request's setting. */
        before=sockopt_calls;
        r=call(cases[i].method,cases[i].path,cases[i].body);
        assert(status(&r)==200 && sockopt_calls==before+1 && r.nodelay_at_first_send==1);

        request_fd=-1;socket_nodelay=0;before=sockopt_calls;
        r=call(cases[i].method,cases[i].path,cases[i].body);
        assert(status(&r)==200 && sockopt_calls==before && r.nodelay_at_first_send==0);
        j=json(&r);authority(j);cJSON_Delete(j);
        request_fd=17;

        fail_sockopt=1;socket_nodelay=0;before=sockopt_calls;
        r=call(cases[i].method,cases[i].path,cases[i].body);
        assert(status(&r)==200 && sockopt_calls==before+1 && r.nodelay_at_first_send==0);
        j=json(&r);authority(j);cJSON_Delete(j);
        fail_sockopt=0;

        /* Exercise the actual product's SDK-error branch, then reuse fd17
         * for a normal response. The mock only supplies SDK socket behavior. */
        fail_guide_snapshot=1;
        r=call(HTTP_GET,"/api/mode",NULL);
        assert(status(&r)==500 && socket_nodelay==0);
        fail_guide_snapshot=0;before=sockopt_calls;
        r=call(cases[i].method,cases[i].path,cases[i].body);
        assert(status(&r)==200 && sockopt_calls==before+1 && r.nodelay_at_first_send==1);
        j=json(&r);authority(j);cJSON_Delete(j);
    }
}
static void rate_contract(void)
{
    const struct {const char *json;bool ok;int q;} cases[]={
        {"{\"rate_quarters\":3,\"rate_half\":2}",true,3},
        {"{\"rate_quarters\":4}",true,4},{"{\"rate_quarters\":0}",true,0},
        {"{\"rate_quarters\":2,\"rate_half\":2}",false,0},
        {"{\"rate_quarters\":3.1}",false,0},{"{\"rate_quarters\":null,\"rate_half\":2}",false,0},
        {"{\"rate_half\":2}",true,4},{"{\"rate_half\":1}",true,2},
        {"{\"rate_half\":1.5}",true,3},{"{\"rate_half\":0.75}",false,0},
        {"{\"rate_half\":-1}",false,0},{"{}",false,0}
    };
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++){
        cJSON *j=cJSON_Parse(cases[i].json);int q=-1;
        assert(focus_rate_quarters(j,&q)==cases[i].ok);
        if(cases[i].ok)assert(q==cases[i].q);
        cJSON_Delete(j);
    }
}
int main(void)
{
    rate_contract();
    assert(controller_modes_init()==ESP_OK);
    assert(mode_http_api_register()==ESP_OK && route_count==5);
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
        if(seq>=2){
            assert(u32(j,"rate_quarters")==3);
            assert(cJSON_GetObjectItemCaseSensitive(j,"rate_half")->valuedouble==1.5);
        }
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
    assert(u32(j,"rate_quarters")==3);cJSON_Delete(j);
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
#ifdef FOCUS_SAMPLED_DEMO
    r=call(HTTP_GET,"/api/mode?compact=1",NULL);
    assert(status(&r)==200 && r.response_size<1024 && r.send_calls==1);
    j=json(&r);authority(j);
    guide=cJSON_GetObjectItemCaseSensitive(j,"task_guide");
    assert(cJSON_IsObject(guide));
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(guide,"state")->valuestring,"ready"));
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(guide,"steps"))==3);
    assert(!strcmp(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(guide,"steps"),0)->valuestring,"丢掉桌面垃圾"));
    assert(!cJSON_HasObjectItem(j,"engagement_diagnostics"));
    cJSON_Delete(j);
#endif
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
    snprintf(body,sizeof(body),"{\"bind\":true,\"controller_boot_id\":%lu,\"session_id\":%lu}",
        (unsigned long)boot,(unsigned long)session);
    nodelay_contract(body);
    /* Finalized facts outlive idle/reset, and ACK cannot cross a boot. */
    assert(controller_modes_stop()==ESP_OK);
    r=call(HTTP_GET,"/api/mode",NULL);assert(status(&r)==200);j=json(&r);
    cJSON *reports=cJSON_GetObjectItemCaseSensitive(j,"focus_reports");
    assert(cJSON_IsObject(reports) && u32(reports,"schema_version")==1);
    cJSON *pending=cJSON_GetObjectItemCaseSensitive(reports,"pending");
    assert(cJSON_GetArraySize(pending)==1);
    cJSON *report=cJSON_GetArrayItem(pending,0);
    assert(u32(report,"session_id")==session && u32(report,"duration_ms")==10800000);
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(report,"end_reason")->valuestring,"stop"));
    assert(!cJSON_HasObjectItem(cJSON_GetObjectItemCaseSensitive(report,"timing_totals"),"half_ms"));
    cJSON_Delete(j);
    snprintf(body,sizeof(body),"{\"controller_boot_id\":%lu,\"session_id\":%lu}",
        (unsigned long)(boot-1),(unsigned long)session);
    r=call(HTTP_POST,"/api/focus/reports/ack",body);assert(status(&r)==409);
    r=call(HTTP_POST,"/api/focus/reports/ack","{}");assert(status(&r)==400);
    r=call_raw(HTTP_POST,"/api/focus/reports/ack","{",1,20);assert(status(&r)==400 && r.close_header);
    snprintf(body,sizeof(body),"{\"controller_boot_id\":%lu,\"session_id\":%lu}",
        (unsigned long)boot,(unsigned long)session);
    r=call(HTTP_POST,"/api/focus/reports/ack",body);assert(status(&r)==200);
    r=call(HTTP_POST,"/api/focus/reports/ack",body);assert(status(&r)==200); /* ACK retry. */
    r=call(HTTP_GET,"/api/mode",NULL);j=json(&r);
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(j,"focus_reports"),"pending"))==0);
    cJSON_Delete(j);
    /* A full eight-report queue must still serialize as complete valid JSON. */
    for(unsigned i=0;i<CONTROLLER_FOCUS_REPORT_CAPACITY+1;i++){
        assert(controller_modes_focus_start(1)==ESP_OK);fake_now+=1000000;
        assert(controller_modes_focus_stop()==ESP_OK);
    }
    r=call(HTTP_GET,"/api/mode",NULL);j=json(&r);
    reports=cJSON_GetObjectItemCaseSensitive(j,"focus_reports");
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(reports,"pending"))==CONTROLLER_FOCUS_REPORT_CAPACITY);
    assert(u32(reports,"dropped_reports")==1);cJSON_Delete(j);
    printf("PASS: actual HTTP handlers + controller_modes + cJSON; compact max=%zu/512, initial full=%zu; "
           "bind/sample authority, residence subtraction, invalid hold400, GET diagnostics, uint32 wrap, stale session409, malformed/truncated/oversized/NUL400; "
           "NODELAY before both normal responses, invalid fd skip, option failure preserves response, error-to-normal socket reuse; "
           "final report retained, 8-entry JSON queue, overflow counter, idempotent ACK, wrong boot409, malformed ACK400\n",
           largest_compact,full_size);
}
