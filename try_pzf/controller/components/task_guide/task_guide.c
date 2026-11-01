#include "task_guide.h"
#include "task_guide_gate.h"
#include "../../../shared/task_guide_protocol.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "controller_modes.h"
#include "esp_log.h"
#include "esp_mcp_data.h"
#include "esp_mcp_property.h"
#include "esp_mcp_tool.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#ifndef CONFIG_TASK_GUIDE_WAIT_TIMEOUT_MS
#define CONFIG_TASK_GUIDE_WAIT_TIMEOUT_MS 30000
#endif

static portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
static struct {
    tg_gate_t gate;
    char task[769], steps[TG_STEP_COUNT][TG_STEP_BYTES];
    char head_state[12];
    uint32_t step, remaining_ms;
    uint64_t prepared_hash;
    size_t prepared_length;
    uint32_t prepared_boot, prepared_request;
} data;

/* This marker is device-authored transport text, never a fresh USER task. */
static const char prepared_prefix[]="[DEVICE_TASK_GUIDE_REQUEST] ";
static uint64_t fingerprint(const char *text)
{
    uint64_t hash=UINT64_C(14695981039346656037);
    for (const unsigned char *p=(const unsigned char *)text;*p;++p) {
        hash^=*p;hash*=UINT64_C(1099511628211);
    }
    return hash;
}

static bool refresh(void)
{
    controller_modes_status_t mode;
    if (controller_modes_get_status(&mode)!=ESP_OK) return false;
    bool ordinary=mode.mode==APP_STATE_IDLE||mode.mode==APP_STATE_COMPANION;
    uint64_t now=(uint64_t)esp_timer_get_time()/1000;
    portENTER_CRITICAL(&lock);
    tg_gate_state_t previous=data.gate.state;
    tg_gate_mode(&data.gate,mode.controller_boot_id,mode.revision,ordinary);
    tg_gate_expire(&data.gate,now,CONFIG_TASK_GUIDE_WAIT_TIMEOUT_MS);
    tg_gate_state_t current=data.gate.state;
    uint32_t request=data.gate.request;
    portEXIT_CRITICAL(&lock);
    if(previous==TG_WAITING&&current==TG_TIMEOUT)
        ESP_LOGW("TASK_GUIDE","Request %lu expired before three validated steps arrived",(unsigned long)request);
    if((previous==TG_WAITING||previous==TG_READY)&&current==TG_DISMISSED)
        ESP_LOGI("TASK_GUIDE","Request %lu dismissed after mode/session change",(unsigned long)request);
    return ordinary;
}

void task_guide_note_user_text(const char *text)
{
    bool intent=tg_request_intent(text);
    if (!refresh()) {
        if(intent)ESP_LOGW("TASK_GUIDE","Request blocked: only ordinary idle/companion modes are supported");
        return;
    }
    /* The cloud STT event can echo HTTP-injected text. Do not let that echo
     * replace its own token, or let a late decorated echo open a new request. */
    if(text&&!strncmp(text,prepared_prefix,sizeof(prepared_prefix)-1)) {
        size_t length=strlen(text);uint64_t hash=fingerprint(text);
        portENTER_CRITICAL(&lock);
        bool echo=data.prepared_length==length&&data.prepared_hash==hash;
        portEXIT_CRITICAL(&lock);
        if(echo) {
            ESP_LOGI("TASK_GUIDE","Exact device-authored task request echo ignored");
            return;
        }
        /* An unknown/obsolete device-authored packet is not a new user intent.
         * Unlike a known echo it follows the normal newer-turn invalidation. */
        intent=false;
    }
    uint64_t now=(uint64_t)esp_timer_get_time()/1000;
    portENTER_CRITICAL(&lock);
    bool duplicate=text&&data.gate.state==TG_WAITING&&!strcmp(text,data.task);
    bool busy=data.gate.state==TG_READY;
    bool opened=!duplicate&&tg_gate_request(&data.gate,intent,now);
    if (opened) {
        memcpy(data.task,text,strlen(text)+1);
        memset(data.steps,0,sizeof(data.steps));
        data.head_state[0]=0; data.step=data.remaining_ms=0;
    }
    uint32_t request=data.gate.request;
    portEXIT_CRITICAL(&lock);
    if (opened) ESP_LOGI("TASK_GUIDE","User request %lu armed (ordinary mode; head owns countdown)",(unsigned long)request);
    else if(duplicate) ESP_LOGI("TASK_GUIDE","Request %lu duplicate/echo retained; token unchanged",(unsigned long)request);
    else if(intent&&busy)ESP_LOGW("TASK_GUIDE","New request blocked: existing guide must finish or exit first");
}

esp_err_t task_guide_prepare_request(const char *text,char *prompt,size_t capacity)
{
    if (!text||!prompt||!capacity) return ESP_ERR_INVALID_ARG;
    if (!tg_request_intent(text)||!strncmp(text,prepared_prefix,sizeof(prepared_prefix)-1)) return ESP_ERR_NOT_FOUND;
    if (!refresh()) return ESP_ERR_INVALID_STATE;
    tg_gate_t before;
    portENTER_CRITICAL(&lock);before=data.gate;portEXIT_CRITICAL(&lock);
    if (before.state==TG_READY) return ESP_ERR_INVALID_STATE;
    uint32_t next=before.request+1;if(!next)++next;
    char token[48];
    snprintf(token,sizeof(token),"%08lx:%lu",(unsigned long)before.boot,(unsigned long)next);
    int n=snprintf(prompt,capacity,
        "%s用户任务：%s\n当前 request_token=\"%s\"。"
        "必须调用 self.task_guide.set，request_token 原样填写，steps 为正好3条具体易懂的动作字符串数组，"
        "每条1至18字，无编号、换行或Markdown，不重复。不要只口述，不要追问。"
        "工具确认后只说“点击屏幕开始”。",
        prepared_prefix,text,token);
    if(n<0||(size_t)n>=capacity) return ESP_ERR_INVALID_SIZE;
    uint64_t hash=fingerprint(prompt),now=(uint64_t)esp_timer_get_time()/1000;
    portENTER_CRITICAL(&lock);
    bool same=data.gate.boot==before.boot&&data.gate.request==before.request&&
        data.gate.state==before.state&&data.gate.mode_revision==before.mode_revision;
    bool opened=same&&tg_gate_request(&data.gate,true,now);
    if(opened) {
        memcpy(data.task,text,strlen(text)+1);memset(data.steps,0,sizeof(data.steps));
        data.head_state[0]=0;data.step=data.remaining_ms=0;
        data.prepared_hash=hash;data.prepared_length=(size_t)n;
        data.prepared_boot=data.gate.boot;data.prepared_request=data.gate.request;
    }
    portEXIT_CRITICAL(&lock);
    if(!opened)return ESP_ERR_INVALID_STATE;
    ESP_LOGI("TASK_GUIDE","HTTP request %lu armed with explicit tool instruction (%u bytes)",
        (unsigned long)next,(unsigned)n);
    return ESP_OK;
}

void task_guide_cancel_prepared_request(const char *prompt)
{
    if(!prompt)return;
    size_t length=strlen(prompt);uint64_t hash=fingerprint(prompt);
    portENTER_CRITICAL(&lock);
    bool cancelled=data.gate.state==TG_WAITING&&data.gate.boot==data.prepared_boot&&
        data.gate.request==data.prepared_request&&data.prepared_length==length&&data.prepared_hash==hash;
    if(cancelled)data.gate.state=TG_DISMISSED;
    uint32_t request=data.gate.request;
    portEXIT_CRITICAL(&lock);
    if(cancelled)ESP_LOGW("TASK_GUIDE","Request %lu cancelled after failed cloud submission",(unsigned long)request);
}

/* No heap work while holding the short state lock. */
cJSON *task_guide_snapshot(void)
{
    refresh();
    tg_gate_t gate;
    char steps[TG_STEP_COUNT][TG_STEP_BYTES],head_state[12];
    uint32_t step,remaining;
    portENTER_CRITICAL(&lock);
    gate=data.gate; memcpy(steps,data.steps,sizeof(steps));
    memcpy(head_state,data.head_state,sizeof(head_state)); step=data.step; remaining=data.remaining_ms;
    portEXIT_CRITICAL(&lock);
    cJSON *j=cJSON_CreateObject();
    if (!j) return NULL;
    cJSON_AddNumberToObject(j,"controller_boot_id",gate.boot);
    cJSON_AddNumberToObject(j,"request_id",gate.request);
    cJSON_AddStringToObject(j,"state",tg_gate_name(gate.state));
    cJSON_AddNumberToObject(j,"wait_timeout_ms",CONFIG_TASK_GUIDE_WAIT_TIMEOUT_MS);
    if (gate.state==TG_READY) {
        cJSON *a=cJSON_AddArrayToObject(j,"steps");
        for (unsigned i=0;i<TG_STEP_COUNT;++i) cJSON_AddItemToArray(a,cJSON_CreateString(steps[i]));
    }
    if (*head_state) {
        cJSON_AddStringToObject(j,"head_state",head_state);
        cJSON_AddNumberToObject(j,"step",step);
        cJSON_AddNumberToObject(j,"remaining_ms",remaining);
        cJSON_AddNumberToObject(j,"report_seq",gate.report_seq);
    }
    if (gate.state==TG_TIMEOUT) cJSON_AddStringToObject(j,"message","暂时没收到结果，再说一次试试");
    return j;
}

static bool u32(const cJSON *j,const char *key,uint32_t *out)
{
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(j,key);
    if (!cJSON_IsNumber(v)||!isfinite(v->valuedouble)||v->valuedouble<0||
        v->valuedouble>UINT32_MAX||floor(v->valuedouble)!=v->valuedouble) return false;
    *out=(uint32_t)v->valuedouble; return true;
}

esp_err_t task_guide_report(const cJSON *j)
{
    uint32_t boot,request,seq,step,remaining;
    const cJSON *state=cJSON_GetObjectItemCaseSensitive(j,"state");
    if (!cJSON_IsObject(j)||!cJSON_IsString(state)||
        !u32(j,"controller_boot_id",&boot)||!u32(j,"request_id",&request)||
        !u32(j,"report_seq",&seq)||!u32(j,"step",&step)||!u32(j,"remaining_ms",&remaining)||
        step>=TG_STEP_COUNT||remaining>180000) return ESP_ERR_INVALID_ARG;
    const char *name=state->valuestring;
    bool done=!strcmp(name,"done"),dismissed=!strcmp(name,"dismissed");
    if (!done&&!dismissed&&strcmp(name,"preview")&&strcmp(name,"running")) return ESP_ERR_INVALID_ARG;
    refresh();
    portENTER_CRITICAL(&lock);
    uint32_t previous=data.gate.report_seq;
    bool terminal=data.gate.state==TG_DONE||data.gate.state==TG_DISMISSED;
    bool ok=tg_gate_report(&data.gate,boot,request,seq,done,dismissed);
    if (ok&&!terminal&&seq>previous) {
        snprintf(data.head_state,sizeof(data.head_state),"%s",name);
        data.step=step;data.remaining_ms=remaining;
    }
    portEXIT_CRITICAL(&lock);
    return ok?ESP_OK:ESP_ERR_INVALID_STATE;
}

static esp_err_t error(esp_mcp_tool_result_t *result,const char *message)
{
    esp_mcp_tool_result_set_is_error(result,true);
    return esp_mcp_tool_result_add_text(result,message);
}

static esp_err_t pending(const esp_mcp_property_list_t *properties,esp_mcp_tool_result_t *result)
{
    (void)properties;
    if (!refresh()) return error(result,"Task guide is available in ordinary idle/companion modes, not focus mode.");
    char task[sizeof(data.task)],token[48];tg_gate_t gate;
    portENTER_CRITICAL(&lock);gate=data.gate;memcpy(task,data.task,sizeof(task));portEXIT_CRITICAL(&lock);
    if (gate.state!=TG_WAITING) {
        ESP_LOGW("TASK_GUIDE","pending rejected: request %lu state=%s",(unsigned long)gate.request,tg_gate_name(gate.state));
        return error(result,"No pending explicit user task request. Do not create a new guide.");
    }
    snprintf(token,sizeof(token),"%08lx:%lu",(unsigned long)gate.boot,(unsigned long)gate.request);
    cJSON *j=cJSON_CreateObject();
    if (!j) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(j,"request_token",token);
    cJSON_AddStringToObject(j,"task",task);
    cJSON_AddStringToObject(j,"instruction","Return exactly three plain action strings, each 1..18 Unicode characters. Call self.task_guide.set with this same token and a steps array; no spoken list.");
    char *text=cJSON_PrintUnformatted(j);cJSON_Delete(j);
    if (!text) return ESP_ERR_NO_MEM;
    esp_err_t ret=esp_mcp_tool_result_add_text(result,text);free(text);return ret;
}

static esp_err_t set_steps(const esp_mcp_property_list_t *properties,esp_mcp_tool_result_t *result)
{
    const char *token=esp_mcp_property_list_get_property_string(properties,"request_token");
    const char *json=esp_mcp_property_list_get_property_array(properties,"steps");
    if (!token||!json||strlen(json)>1024||strstr(json,"\\u0000")) return error(result,"Expected request_token and steps array of exactly three plain strings without NUL.");
    if (!refresh()) return error(result,"Ordinary idle/companion mode is required; focus is not supported.");
    cJSON *a=cJSON_ParseWithOpts(json,NULL,true);
    const char *steps[TG_STEP_COUNT]={0};
    if (cJSON_IsArray(a)&&cJSON_GetArraySize(a)==TG_STEP_COUNT) {
        for (unsigned i=0;i<TG_STEP_COUNT;++i) {
            const cJSON *v=cJSON_GetArrayItem(a,i);
            if (cJSON_IsString(v)) steps[i]=v->valuestring;
        }
    }
    const char *why=tg_steps_error(steps);
    if (why) {ESP_LOGW("TASK_GUIDE","set rejected: %s",why);cJSON_Delete(a);return error(result,why);}
    char expected[48];bool accepted=false;
    portENTER_CRITICAL(&lock);
    snprintf(expected,sizeof(expected),"%08lx:%lu",(unsigned long)data.gate.boot,(unsigned long)data.gate.request);
    if (!strcmp(token,expected)&&tg_gate_accept(&data.gate,data.gate.boot,data.gate.request)) {
        for (unsigned i=0;i<TG_STEP_COUNT;++i) memcpy(data.steps[i],steps[i],strlen(steps[i])+1);
        accepted=true;
    }
    portEXIT_CRITICAL(&lock);
    cJSON_Delete(a);
    if (!accepted) {
        ESP_LOGW("TASK_GUIDE","set rejected: stale token, expired request, or guide already delivered");
        return error(result,"Request expired, was dismissed, or token is stale. Do not retry this request or open another guide.");
    }
    ESP_LOGI("TASK_GUIDE","Three validated steps accepted; waiting for head preview report");
    return esp_mcp_tool_result_add_text(result,"Three steps accepted for head preview. Countdown starts only after a screen tap. Say only: 三步已准备好，点击屏幕开始。 Do not claim the screen or countdown has already started.");
}

esp_err_t task_guide_register_mcp_tools(esp_mcp_t *engine)
{
    if (!engine) return ESP_ERR_INVALID_ARG;
    esp_mcp_tool_t *p=esp_mcp_tool_create_ex("self.task_guide.pending",NULL,
        "用户要求拆解/拆分/分解任务，或说分3步/分三步、提醒我怎么做时调用。读取当前请求令牌和原始任务；只读，不创建任务。再用同一令牌调用self.task_guide.set。",pending);
    if (!p) return ESP_ERR_NO_MEM;
    esp_err_t ret=esp_mcp_add_tool(engine,p);
    if (ret!=ESP_OK) {esp_mcp_tool_destroy(p);return ret;}
    esp_mcp_tool_t *s=esp_mcp_tool_create_ex("self.task_guide.set",NULL,
        "仅完成设备已经打开的当前请求。request_token必须原样使用设备提示或self.task_guide.pending给出的值。steps必须正好3个简短易懂的具体动作字符串，每条1至18字符，无编号、换行或Markdown，不重复。设备校验后才显示，等待点击开始每步3分钟。不要在语音中罗列三步。",set_steps);
    if (!s) return ESP_ERR_NO_MEM;
    esp_mcp_property_t *t=esp_mcp_property_create("request_token",ESP_MCP_PROPERTY_TYPE_STRING);
    esp_mcp_property_t *a=esp_mcp_property_create("steps",ESP_MCP_PROPERTY_TYPE_ARRAY);
    if (!t||!a) {if(t)esp_mcp_property_destroy(t);if(a)esp_mcp_property_destroy(a);esp_mcp_tool_destroy(s);return ESP_ERR_NO_MEM;}
    ret=esp_mcp_tool_add_property(s,t);
    if (ret!=ESP_OK) {esp_mcp_property_destroy(t);esp_mcp_property_destroy(a);esp_mcp_tool_destroy(s);return ret;}
    ret=esp_mcp_tool_add_property(s,a);
    if (ret!=ESP_OK) {esp_mcp_property_destroy(a);esp_mcp_tool_destroy(s);return ret;}
    ret=esp_mcp_add_tool(engine,s);
    if(ret!=ESP_OK)esp_mcp_tool_destroy(s);
    else ESP_LOGI("TASK_GUIDE","MCP tools registered: self.task_guide.pending, self.task_guide.set; modes=idle/companion wait_ms=%u",CONFIG_TASK_GUIDE_WAIT_TIMEOUT_MS);
    return ret;
}
