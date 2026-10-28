#include "mcp_vision_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "cJSON.h"

#define URL_SIZE 128
#define RESPONSE_SIZE 12288
static portMUX_TYPE s_lock=portMUX_INITIALIZER_UNLOCKED;
static char s_url[URL_SIZE];
static const char *TAG="MCP_VISION";

esp_err_t mcp_vision_set_url(const char *url)
{
    if (!url || strlen(url)>=URL_SIZE || strncmp(url,"http://",7)) return ESP_ERR_INVALID_ARG;
    unsigned a,b,c,d,port=80; int n=0,tail=0;
    if (sscanf(url+7,"%u.%u.%u.%u%n",&a,&b,&c,&d,&n)!=4 ||
        a>255 || b>255 || c>255 || d>255 || !d || d==255) return ESP_ERR_INVALID_ARG;
    const char *end=url+7+n;
    if (*end==':') {
        if (sscanf(end+1,"%u%n",&port,&tail)!=1 || !port || port>65535) return ESP_ERR_INVALID_ARG;
        end+=1+tail;
    }
    if (*end=='/') ++end;
    bool lan=a==10 || (a==172 && b>=16 && b<=31) || (a==192 && b==168);
    if (*end || !lan) return ESP_ERR_INVALID_ARG;
    char normalized[URL_SIZE];
    if (port==80) snprintf(normalized,sizeof(normalized),"http://%u.%u.%u.%u",a,b,c,d);
    else snprintf(normalized,sizeof(normalized),"http://%u.%u.%u.%u:%u",a,b,c,d,port);
    portENTER_CRITICAL(&s_lock);
    memcpy(s_url,normalized,strlen(normalized)+1);
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG,"PC service registered: %s (RAM only)",normalized);
    return ESP_OK;
}
void mcp_vision_get_url(char *out,size_t size)
{
    if (!out || !size) return;
    portENTER_CRITICAL(&s_lock);
    snprintf(out,size,"%s",s_url);
    portEXIT_CRITICAL(&s_lock);
}
typedef struct { char *data; size_t used; bool overflow; mcp_probe_guard_t current; void *context; } response_t;
static esp_err_t http_event(esp_http_client_event_t *event)
{
    response_t *r=event->user_data;
    if (!r->current(r->context)) return ESP_FAIL;
    if (event->event_id==HTTP_EVENT_ON_DATA) {
        if (event->data_len<0 || r->used+(size_t)event->data_len>=RESPONSE_SIZE) {
            r->overflow=true; return ESP_FAIL;
        }
        memcpy(r->data+r->used,event->data,event->data_len); r->used+=event->data_len;
    }
    return ESP_OK;
}
static cJSON *request_json(const char *base,const char *path,const char *body,int *status,
                           mcp_probe_guard_t current,void *context,int timeout_ms)
{
    if (!current(context)) return NULL;
    char url[256];
    if (snprintf(url,sizeof(url),"%s%s",base,path)>=(int)sizeof(url)) return NULL;
    response_t r={.data=calloc(1,RESPONSE_SIZE),.current=current,.context=context};
    if (!r.data) return NULL;
    esp_http_client_config_t config={.url=url,.timeout_ms=timeout_ms,.event_handler=http_event,
        .user_data=&r,.disable_auto_redirect=true};
    esp_http_client_handle_t client=esp_http_client_init(&config);
    if (!client) { free(r.data); return NULL; }
    if (body) {
        esp_http_client_set_method(client,HTTP_METHOD_POST);
        esp_http_client_set_header(client,"Content-Type","application/json");
        esp_http_client_set_post_field(client,body,strlen(body));
    }
    esp_err_t ret=esp_http_client_perform(client); /* Exactly once, no POST retry. */
    *status=esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    cJSON *json=ret==ESP_OK && !r.overflow && current(context) ? cJSON_Parse(r.data) : NULL;
    free(r.data);
    return json;
}
static char *text_copy(const char *text) { return strdup(text); }
static const cJSON *item(const cJSON *root,const char *key) { return cJSON_GetObjectItemCaseSensitive(root,key); }
static bool valid_id(const char *id)
{
    if (!id || !*id || strlen(id)>64) return false;
    for (const char *p=id;*p;++p) if (!isxdigit((unsigned char)*p) && *p!='-') return false;
    return true;
}
static char *service_error(const cJSON *root,const char *fallback)
{
    const cJSON *message=item(root,"error");
    return text_copy(cJSON_IsString(message) && strlen(message->valuestring)<400 ? message->valuestring : fallback);
}
char *mcp_vision_answer(const char *question,mcp_probe_guard_t current,void *context,bool *failed)
{
    *failed=true;
    char base[URL_SIZE];mcp_vision_get_url(base,sizeof(base));
    if (!*base) return text_copy("电脑识图地址尚未注册，请在网页连接真实设备后再问。");
    int status=0;
    cJSON *json=request_json(base,"/input_source",NULL,&status,current,context,2500);
    bool reachable=status==200 && cJSON_IsTrue(item(json,"success")) && cJSON_IsString(item(json,"current_source"));
    cJSON_Delete(json);
    if (!reachable) return text_copy("控制器暂时无法访问电脑识图服务，请检查电脑与台灯的局域网连接。");
    json=request_json(base,"/vqa/health",NULL,&status,current,context,2500);
    bool usable=status==200 && cJSON_IsTrue(item(json,"success")) && cJSON_IsTrue(item(json,"configured"));
    bool busy=cJSON_IsTrue(item(json,"processing"));
    cJSON_Delete(json);
    if (!usable) return text_copy("电脑服务可达，但视觉问答服务未就绪或未配置。");
    if (busy) return text_copy("视觉问答正在处理其他问题，请稍后再问；本次没有排队。");
    cJSON *body=cJSON_CreateObject();
    if (!body || !cJSON_AddStringToObject(body,"question",question)) {
        cJSON_Delete(body);return text_copy("识图请求内存不足。");
    }
    char *encoded=cJSON_PrintUnformatted(body);cJSON_Delete(body);
    if (!encoded) return text_copy("识图请求内存不足。");
    int64_t started=esp_timer_get_time();
    json=request_json(base,"/vqa/recognize",encoded,&status,current,context,5000);
    cJSON_free(encoded);
    if (!json) return text_copy("识图提交结果未知；没有自动重发，请稍后再问。");
    if (status!=202 && status!=200) {
        char *message=service_error(json,"无法受理识图请求。");cJSON_Delete(json);return message;
    }
    const cJSON *id=item(json,"request_id");
    char request_id[65]={0};
    if (!cJSON_IsString(id) || !valid_id(id->valuestring)) { cJSON_Delete(json);return text_copy("识图服务返回的请求编号无效。"); }
    snprintf(request_id,sizeof(request_id),"%s",id->valuestring);
    ESP_LOGI(TAG,"accepted VQA request_id=%s (question only; image stays on PC)",request_id);
    while (current(context)) {
        const cJSON *record=item(json,"result"), *rid=item(record,"request_id"), *state=item(record,"state");
        if (!cJSON_IsString(rid) || strcmp(rid->valuestring,request_id) || !cJSON_IsString(state)) {
            cJSON_Delete(json);return text_copy("识图结果编号或状态不匹配，已放弃本次结果。");
        }
        if (!strcmp(state->valuestring,"done")) {
            const cJSON *answer=item(record,"answer");
            char *text=NULL;
            if (cJSON_IsString(answer) && *answer->valuestring && strlen(answer->valuestring)<=8192) {
                /* Metadata stays with the answer; this is the frozen frame, not the live view. */
                cJSON *result=cJSON_CreateObject();
                if (result && cJSON_AddStringToObject(result,"answer",answer->valuestring)) {
                    const char *keys[]={"frame_seq","stream_id","sampled_at"};
                    for (int i=0;i<3;i++) {
                        const cJSON *value=item(record,keys[i]);
                        if (value) cJSON_AddItemToObject(result,keys[i],cJSON_Duplicate(value,true));
                    }
                    text=cJSON_PrintUnformatted(result);
                }
                cJSON_Delete(result);
                if (text) *failed=false;
            }
            ESP_LOGI(TAG,"completed request_id=%s elapsed_ms=%lld",request_id,(long long)((esp_timer_get_time()-started)/1000));
            cJSON_Delete(json);return text ? text : text_copy("识图返回了空答案或答案过长。");
        }
        if (!strcmp(state->valuestring,"failed")) {
            char *text=service_error(record,"视觉问答失败。");cJSON_Delete(json);return text;
        }
        if (strcmp(state->valuestring,"running")) { cJSON_Delete(json);return text_copy("识图服务返回未知状态。"); }
        cJSON_Delete(json);json=NULL;
        if (esp_timer_get_time()-started>=30000000LL) return text_copy("识别等待超时，本次结果未知；没有自动重发。");
        for (int i=0;i<25 && current(context);i++) vTaskDelay(pdMS_TO_TICKS(20));
        char path[128];snprintf(path,sizeof(path),"/vqa/state?request_id=%s",request_id);
        json=request_json(base,path,NULL,&status,current,context,2500);
        if (!json || status!=200) {
            char *text=service_error(json,"识图状态查询失败，本次结果未知；没有自动重发。");
            cJSON_Delete(json);return text;
        }
    }
    cJSON_Delete(json);
    return text_copy("原语音会话已结束，已丢弃识图结果。");
}
