/* Production task_guide.c with SDK boundary fixtures, not a copied gate model. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "task_guide.h"
#include "controller_modes.h"
#include "esp_mcp_tool.h"

struct esp_mcp_s { unsigned count;struct esp_mcp_tool_s *tools[2]; };
struct esp_mcp_property_s { esp_mcp_property_type_t type; };
struct esp_mcp_property_list_s { const char *token,*steps; };
struct esp_mcp_tool_s { const char *name;esp_mcp_tool_callback_ex_t callback;unsigned properties;esp_mcp_property_t *owned[2]; };
struct esp_mcp_tool_result_s { bool is_error;char text[1800]; };
static controller_modes_status_t mode={.mode=APP_STATE_IDLE,.controller_boot_id=41};
static int64_t now_ms;
void task_guide_host_log(const char *tag,const char *format,...) { (void)tag;(void)format; }
int64_t esp_timer_get_time(void) {return now_ms*1000;}
esp_err_t controller_modes_get_status(controller_modes_status_t *out){*out=mode;return ESP_OK;}
esp_mcp_tool_t *esp_mcp_tool_create_ex(const char *name,const char *title,const char *description,esp_mcp_tool_callback_ex_t cb) {
    (void)title;(void)description;esp_mcp_tool_t *t=calloc(1,sizeof(*t));assert(t);t->name=name;t->callback=cb;return t;
}
esp_err_t esp_mcp_add_tool(esp_mcp_t *engine,esp_mcp_tool_t *tool){assert(engine->count<2);engine->tools[engine->count++]=tool;return ESP_OK;}
esp_err_t esp_mcp_tool_destroy(esp_mcp_tool_t *t){for(unsigned i=0;i<t->properties;i++)free(t->owned[i]);free(t);return ESP_OK;}
esp_mcp_property_t *esp_mcp_property_create(const char *name,esp_mcp_property_type_t type){(void)name;esp_mcp_property_t *p=malloc(sizeof(*p));assert(p);p->type=type;return p;}
esp_err_t esp_mcp_property_destroy(esp_mcp_property_t *p){free(p);return ESP_OK;}
esp_err_t esp_mcp_tool_add_property(esp_mcp_tool_t *t,esp_mcp_property_t *p){assert(t->properties<2);t->owned[t->properties++]=p;return ESP_OK;}
const char *esp_mcp_property_list_get_property_string(const esp_mcp_property_list_t *p,const char *name){(void)name;return p?p->token:NULL;}
const char *esp_mcp_property_list_get_property_array(const esp_mcp_property_list_t *p,const char *name){(void)name;return p?p->steps:NULL;}
esp_err_t esp_mcp_tool_result_set_is_error(esp_mcp_tool_result_t *r,bool value){r->is_error=value;return ESP_OK;}
esp_err_t esp_mcp_tool_result_add_text(esp_mcp_tool_result_t *r,const char *text){int n=snprintf(r->text,sizeof(r->text),"%s",text);assert(n>0&&(size_t)n<sizeof(r->text));return ESP_OK;}

static unsigned request(const char *state) {
    cJSON *snapshot=task_guide_snapshot();assert(snapshot);
    assert(!strcmp(cJSON_GetObjectItem(snapshot,"state")->valuestring,state));
    unsigned result=(unsigned)cJSON_GetObjectItem(snapshot,"request_id")->valuedouble;
    cJSON_Delete(snapshot);return result;
}
static void token_from_pending(esp_mcp_t *engine,char *token,size_t capacity) {
    esp_mcp_tool_result_t result={0};assert(engine->tools[0]->callback(NULL,&result)==ESP_OK&&!result.is_error);
    cJSON *j=cJSON_Parse(result.text);assert(j);snprintf(token,capacity,"%s",cJSON_GetObjectItem(j,"request_token")->valuestring);cJSON_Delete(j);
}
int main(void) {
    esp_mcp_t engine={0};assert(task_guide_register_mcp_tools(&engine)==ESP_OK);
    assert(engine.count==2&&engine.tools[1]->owned[1]->type==ESP_MCP_PROPERTY_TYPE_ARRAY);
    const char *user="清理桌面分3步，提醒我怎么做";
    task_guide_note_user_text(user);assert(request("waiting")==1); // actual logs run mode=idle
    task_guide_note_user_text(user);assert(request("waiting")==1); // original STT echo
    char prompt[TASK_GUIDE_REQUEST_PROMPT_BYTES],token[48];
    assert(task_guide_prepare_request("你好",prompt,sizeof(prompt))==ESP_ERR_NOT_FOUND);
    assert(request("waiting")==1);
    assert(task_guide_prepare_request(user,prompt,20)==ESP_ERR_INVALID_SIZE);
    assert(request("waiting")==1); // overflow did not open a hidden request
    assert(task_guide_prepare_request(user,prompt,sizeof(prompt))==ESP_OK);
    assert(request("waiting")==2&&strstr(prompt,"self.task_guide.set"));
    token_from_pending(&engine,token,sizeof(token));assert(strstr(prompt,token));
    task_guide_note_user_text(prompt);assert(request("waiting")==2);
    task_guide_note_user_text(user);assert(request("waiting")==2);
    esp_mcp_property_list_t props={.token=token,.steps="[\"一步\",\"二步\"]"};
    esp_mcp_tool_result_t result={0};assert(engine.tools[1]->callback(&props,&result)==ESP_OK&&result.is_error);
    assert(request("waiting")==2);
    props.steps="[\"丢掉垃圾\",\"分类收好物品\",\"擦净桌面\"]";result=(esp_mcp_tool_result_t){0};
    assert(engine.tools[1]->callback(&props,&result)==ESP_OK&&!result.is_error);assert(request("ready")==2);
    task_guide_cancel_prepared_request(prompt);assert(request("ready")==2);
    assert(task_guide_prepare_request(user,prompt,sizeof(prompt))==ESP_ERR_INVALID_STATE);
    task_guide_note_user_text("你好");assert(request("ready")==2);
    cJSON *report=cJSON_Parse("{\"controller_boot_id\":41,\"request_id\":2,\"report_seq\":1,\"state\":\"dismissed\",\"step\":0,\"remaining_ms\":0}");
    assert(task_guide_report(report)==ESP_OK);cJSON_Delete(report);assert(request("dismissed")==2);
    task_guide_note_user_text(prompt);assert(request("dismissed")==2); // late prepared echo never reopens
    assert(task_guide_prepare_request(user,prompt,sizeof(prompt))==ESP_OK);assert(request("waiting")==3);
    task_guide_cancel_prepared_request("other text");assert(request("waiting")==3);
    task_guide_cancel_prepared_request(prompt);assert(request("dismissed")==3);
    mode.mode=APP_STATE_FOCUS;mode.revision++;
    assert(task_guide_prepare_request(user,prompt,sizeof(prompt))==ESP_ERR_INVALID_STATE);
    task_guide_note_user_text(user);assert(request("dismissed")==3);
    mode.mode=APP_STATE_COMPANION;mode.revision++;
    assert(task_guide_prepare_request(user,prompt,sizeof(prompt))==ESP_OK);assert(request("waiting")==4);
    now_ms=30000;assert(request("timeout")==4);
    result=(esp_mcp_tool_result_t){0};assert(engine.tools[0]->callback(NULL,&result)==ESP_OK&&result.is_error);
    for(unsigned i=0;i<engine.count;i++)esp_mcp_tool_destroy(engine.tools[i]);
    puts("PASS: production task guide idle/STT/HTTP prompt/token echo/MCP steps/mode/timeout paths");
}
