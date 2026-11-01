#include "focus_sync.h"
#include "focus_sync_recovery.h"
#include "../../../shared/focus_transport_age.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "focuslamp_discovery.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_EXAMPLE_ENABLE_DISPLAY
#include "task_guide.h"
#endif

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static focus_view_t view;
static bool started;
typedef struct {
    uint32_t received, accepted, malformed, clock_rejected, source_rejected;
    uint32_t sample_seq, inference_seq, input_age_ms, input_sample_age_ms, transit_ms;
    uint32_t echo_elapsed_ms, echo_hold_ms;
    uint32_t sent_seq, sent_age_ms, sent_sample_age_ms, http_ms, exchanges, failures;
    int http_status, request_kind;
} focus_diag_t;
static focus_diag_t diag;
cJSON *focus_sync_diagnostics(void)
{
    focus_diag_t d;
    portENTER_CRITICAL(&lock); d=diag; portEXIT_CRITICAL(&lock);
    cJSON *j=cJSON_CreateObject(); if(!j)return NULL;
#define ADD(field) cJSON_AddNumberToObject(j,#field,d.field)
    cJSON_AddNumberToObject(j,"version",1);
    ADD(received);ADD(accepted);ADD(malformed);ADD(clock_rejected);ADD(source_rejected);
    ADD(sample_seq);ADD(inference_seq);ADD(input_age_ms);ADD(input_sample_age_ms);ADD(transit_ms);
    ADD(echo_elapsed_ms);ADD(echo_hold_ms);
    ADD(sent_seq);ADD(sent_age_ms);ADD(sent_sample_age_ms);ADD(http_ms);ADD(exchanges);ADD(failures);
    ADD(http_status);ADD(request_kind);
#undef ADD
    return j;
}
typedef struct {
    int fd;
    uint32_t generation, seq, inference, age, sample_age;
    bool valid, present;
    char epoch[80], level[40];
    int64_t received;
} sample_t;
static sample_t sample = {.fd=-1};
void focus_sync_source(int fd, const char *epoch)
{
    if (!epoch || !*epoch || strlen(epoch)>=sizeof(sample.epoch)) return;
    portENTER_CRITICAL(&lock);
    uint32_t generation=sample.generation+1;
    sample=(sample_t){.fd=fd,.generation=generation};
    snprintf(sample.epoch,sizeof(sample.epoch),"%s",epoch);
    portEXIT_CRITICAL(&lock);
}
static bool number(const cJSON *j,const char *key,uint32_t *out)
{
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(j,key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble<0 ||
        v->valuedouble>UINT32_MAX || floor(v->valuedouble)!=v->valuedouble) return false;
    *out=(uint32_t)v->valuedouble; return true;
}
void focus_sync_sample(int fd,const cJSON *j)
{
    portENTER_CRITICAL(&lock); ++diag.received; portEXIT_CRITICAL(&lock);
    sample_t next={0}; uint32_t echoed, hold=0, transit;
    const cJSON *epoch=cJSON_GetObjectItemCaseSensitive(j,"source_epoch");
    const cJSON *level=cJSON_GetObjectItemCaseSensitive(j,"engage_level_name");
    const cJSON *valid=cJSON_GetObjectItemCaseSensitive(j,"valid");
    if (!cJSON_IsString(epoch)||!cJSON_IsString(level)||!cJSON_IsBool(valid)||
        !number(j,"sample_seq",&next.seq)||!number(j,"inference_seq",&next.inference)||
        !number(j,"age_ms",&next.age)||!number(j,"sample_age_ms",&next.sample_age)||!number(j,"head_clock_ms",&echoed)) {
        portENTER_CRITICAL(&lock);++diag.malformed;portEXIT_CRITICAL(&lock);return;
    }
    if (cJSON_GetObjectItemCaseSensitive(j,"head_clock_hold_ms") && !number(j,"head_clock_hold_ms",&hold)) {
        portENTER_CRITICAL(&lock);++diag.malformed;portEXIT_CRITICAL(&lock);return;
    }
    portENTER_CRITICAL(&lock);
    diag.sample_seq=next.seq;diag.inference_seq=next.inference;
    diag.input_age_ms=next.age;diag.input_sample_age_ms=next.sample_age;
    diag.echo_elapsed_ms=(uint32_t)(esp_timer_get_time()/1000)-echoed;diag.echo_hold_ms=hold;
    portEXIT_CRITICAL(&lock);
    if (!focus_transport_age((uint32_t)(esp_timer_get_time()/1000),echoed,hold,&transit)) {
        portENTER_CRITICAL(&lock);++diag.clock_rejected;portEXIT_CRITICAL(&lock);return;
    }
    next.age=focus_age_add(next.age,transit);
    next.sample_age=focus_age_add(next.sample_age,transit);
    portENTER_CRITICAL(&lock);
    diag.transit_ms=transit;
    if (fd==sample.fd && !strcmp(epoch->valuestring,sample.epoch) && (!sample.present || next.seq>sample.seq)) {
        ++diag.accepted;
        sample.seq=next.seq; sample.inference=next.inference; sample.age=next.age;
        sample.sample_age=next.sample_age; sample.valid=cJSON_IsTrue(valid); sample.present=true;
        sample.received=esp_timer_get_time();
        const char *name=level->valuestring;
        if(strcmp(name,"HighlyEngaged") && strcmp(name,"Engaged") && strcmp(name,"BarelyEngaged") && strcmp(name,"NotEngaged")) name="Unknown";
        snprintf(sample.level,sizeof(sample.level),"%s",name);
    } else ++diag.source_rejected;
    portEXIT_CRITICAL(&lock);
}
void focus_sync_get_view(focus_view_t *out)
{
    portENTER_CRITICAL(&lock); *out=view; portEXIT_CRITICAL(&lock);
    out->fresh=out->fresh && esp_timer_get_time()-out->received_us<2000000;
}
typedef struct { char data[3072]; size_t size; bool overflow; } response_t;
static esp_err_t collect(esp_http_client_event_t *event)
{
    response_t *r=event->user_data;
    if(event->event_id==HTTP_EVENT_ON_DATA) {
        if(r->size+event->data_len>=sizeof(r->data)){r->overflow=true;return ESP_FAIL;}
        memcpy(r->data+r->size,event->data,event->data_len);r->size+=event->data_len;r->data[r->size]=0;
    }
    return ESP_OK;
}
static cJSON *exchange_path(const focuslamp_endpoint_t *peer,const char *path,const char *body,int *status)
{
    /* Only the serialized focus worker calls this function. Preserve HTTP
     * keep-alive across mode/sample requests; never retain a failed transport
     * or reuse a socket after discovery selects a different controller. */
    static esp_http_client_handle_t client;
    static char host[64];static uint16_t port;
    *status=0;
    char url[160];snprintf(url,sizeof(url),"http://%s:%u/api/mode%s",peer->host,peer->port,path);
    response_t response={0};
    if(client && (strcmp(host,peer->host)||port!=peer->port)){
        esp_http_client_cleanup(client);client=NULL;
    }
    if(!client){
        esp_http_client_config_t cfg={.url=url,.timeout_ms=900,.event_handler=collect,.user_data=&response,
            .method=body?HTTP_METHOD_POST:HTTP_METHOD_GET};
        client=esp_http_client_init(&cfg);
        if(client){snprintf(host,sizeof(host),"%s",peer->host);port=peer->port;}
    }
    if(!client)return NULL;
    esp_http_client_set_user_data(client,&response);
    if(esp_http_client_set_url(client,url)!=ESP_OK ||
       esp_http_client_set_method(client,body?HTTP_METHOD_POST:HTTP_METHOD_GET)!=ESP_OK ||
       esp_http_client_set_post_field(client,body?body:"",body?strlen(body):0)!=ESP_OK){
        esp_http_client_cleanup(client);client=NULL;return NULL;
    }
    if(body)esp_http_client_set_header(client,"Content-Type","application/json");
    else esp_http_client_delete_header(client,"Content-Type");
    esp_err_t ret=esp_http_client_perform(client);int code=esp_http_client_get_status_code(client);
    if(ret==ESP_OK)*status=code; /* Partial/failed transport is not an HTTP rejection. */
    /* response and body belong to this call's stack, not the retained handle. */
    esp_http_client_set_user_data(client,NULL);
    esp_http_client_set_post_field(client,"",0);
    if(ret!=ESP_OK || response.overflow){esp_http_client_cleanup(client);client=NULL;}
    return ret==ESP_OK && code==200 && !response.overflow ? cJSON_Parse(response.data):NULL;
}
static void worker(void *unused)
{
    (void)unused;
    uint32_t boot=0,session=0,token=0,generation=0,sent=0,observed=0;
    int64_t observed_received_us=0;
    focus_sync_recovery_t recovery={.probe=true};
    char bound_host[16]={0}; uint16_t bound_port=0; bool active=false;
    for(;;){
        focuslamp_endpoint_t peer;
        sample_t data;
        portENTER_CRITICAL(&lock);data=sample;portEXIT_CRITICAL(&lock);
        cJSON *reply=NULL;
        int response_status=0;
        int64_t reply_received_us=0;
        bool binding=false;
        int64_t begin=esp_timer_get_time();
        if(focuslamp_discovery_get_peer(FOCUSLAMP_ROLE_CONTROLLER,&peer)==ESP_OK){
            if(strcmp(peer.host,bound_host)||peer.port!=bound_port){
                token=boot=0;recovery=(focus_sync_recovery_t){.probe=true};
                snprintf(bound_host,sizeof(bound_host),"%s",peer.host);bound_port=peer.port;
            }
            char body[512];const char *payload=NULL;
            if(!recovery.probe && active && boot && data.present && (!token || generation!=data.generation)){
                binding=true;
                focus_sync_bind_started(&recovery,data.generation,token);
                snprintf(body,sizeof(body),"{\"bind\":true,\"controller_boot_id\":%lu,\"session_id\":%lu}",
                    (unsigned long)boot,(unsigned long)session);payload=body;
            }else if(!recovery.probe && active && boot && token && data.present && data.seq>sent){
                /* Controller measures a conservative round trip from its own echoed
                 * monotonic timestamp. This task adds only local residence. */
                uint64_t wait=(begin-data.received)/1000;
                uint64_t hold=(begin-observed_received_us)/1000;
                uint64_t age=(uint64_t)data.age+wait, frame_age=(uint64_t)data.sample_age+wait;
                snprintf(body,sizeof(body),"{\"observed_ms\":%lu,\"observed_hold_ms\":%lu,\"controller_boot_id\":%lu,\"session_id\":%lu,\"engagement_token\":%lu,"
                    "\"sample_seq\":%lu,\"inference_seq\":%lu,\"age_ms\":%lu,\"sample_age_ms\":%lu,\"valid\":%s,\"level\":\"%s\"}",
                    (unsigned long)observed,(unsigned long)(hold>UINT32_MAX?UINT32_MAX:hold),
                    (unsigned long)boot,(unsigned long)session,(unsigned long)token,(unsigned long)data.seq,
                    (unsigned long)data.inference,(unsigned long)(age>UINT32_MAX?UINT32_MAX:age),
                    (unsigned long)(frame_age>UINT32_MAX?UINT32_MAX:frame_age),data.valid?"true":"false",data.level);
                payload=body;sent=data.seq; /* Never replay a timed-out sample. */
            }
            int64_t request_started_us=esp_timer_get_time();
            reply=exchange_path(&peer,payload?"/engagement":"",payload,&response_status);
            reply_received_us=esp_timer_get_time();
            portENTER_CRITICAL(&lock);
            ++diag.exchanges;if(!reply)++diag.failures;
            diag.http_status=response_status;diag.request_kind=payload?(binding?1:2):0;
            diag.http_ms=(uint32_t)((reply_received_us-request_started_us)/1000);
            if(payload && !binding){
                diag.sent_seq=data.seq;
                diag.sent_age_ms=focus_age_add(data.age,(uint32_t)((begin-data.received)/1000));
                diag.sent_sample_age_ms=focus_age_add(data.sample_age,(uint32_t)((begin-data.received)/1000));
            }
            portEXIT_CRITICAL(&lock);
#if CONFIG_EXAMPLE_ENABLE_DISPLAY
            if(reply)task_guide_observe_mode(reply);
            char guide_report[320];uint32_t report_seq=0;
            if(task_guide_report(guide_report,sizeof(guide_report),&report_seq)) {
                int report_status=0;
                cJSON *ack=exchange_path(&peer,"/task_guide",guide_report,&report_status);
                // 409 means the controller has moved to another request/boot; never replay obsolete reports.
                if(report_status==200 || report_status==409)task_guide_report_ack(report_seq);
                cJSON_Delete(ack);
            }
#endif
        }
        focus_view_t next={0};uint32_t next_token=0,next_observed=0;
        const cJSON *mode=cJSON_GetObjectItemCaseSensitive(reply,"mode");
        const cJSON *reason=cJSON_GetObjectItemCaseSensitive(reply,"pause_reason");
        uint32_t rate=0;
        bool ok=cJSON_IsString(mode)&&cJSON_IsString(reason)&&number(reply,"controller_boot_id",&next.boot)&&
            number(reply,"uptime_ms",&next_observed)&&number(reply,"session_id",&next.session)&&number(reply,"seq",&next.seq)&&
            number(reply,"remaining_ms",&next.remaining_ms)&&number(reply,"duration_ms",&next.duration_ms)&&
            number(reply,"rate_half",&rate)&&rate<=2&&number(reply,"engagement_token",&next_token);
        if(ok){
            next.focus=!strcmp(mode->valuestring,"focus");next.completed=cJSON_IsTrue(cJSON_GetObjectItem(reply,"completed"));
            next.paused=cJSON_IsTrue(cJSON_GetObjectItem(reply,"paused"));next.rate_half=rate;
            next.fresh=true;next.received_us=reply_received_us;
            snprintf(next.reason,sizeof(next.reason),"%s",reason->valuestring);
            portENTER_CRITICAL(&lock);
            /* A serialized HTTP worker is the connection generation: no old
             * in-flight reply can overtake the next resynchronization. */
            if(view.boot!=next.boot||view.session!=next.session||next.seq>view.seq){
                if(view.boot==next.boot&&view.session==next.session&&next.remaining_ms>view.remaining_ms) ok=false;
                else view=next;
            }
            portEXIT_CRITICAL(&lock);
            if(ok){
                if(boot!=next.boot||session!=next.session){token=0;sent=0;}
                else if(focus_sync_bind_confirmed(&recovery,binding,data.generation,next_token)){
                    token=next_token;generation=data.generation;sent=0;
                }else if(token && next_token!=token){token=0;}
                if(!next.focus||next.completed)token=0;
                recovery=(focus_sync_recovery_t){0};
                observed=next_observed;observed_received_us=next.received_us;
                boot=next.boot;session=next.session;active=next.focus && !next.completed;
            }
        }
        if(!ok && focus_sync_failed(&recovery,response_status)){token=boot=0;}
        if(!ok){portENTER_CRITICAL(&lock);view.fresh=false;portEXIT_CRITICAL(&lock);}
        cJSON_Delete(reply);
        /* No backlog; one request in flight, no catch-up loops. */
        int64_t elapsed=esp_timer_get_time()-begin;
        vTaskDelay(pdMS_TO_TICKS(elapsed<500000?(500000-elapsed)/1000:50));
    }
}
esp_err_t focus_sync_start(void)
{
    if(started)return ESP_OK;
    if(xTaskCreate(worker,"focus_sync",10240,NULL,3,NULL)!=pdPASS)return ESP_ERR_NO_MEM;
    started=true;return ESP_OK;
}
