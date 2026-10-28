#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_client.h"
#include "mcp_vision_client.h"
#define EXPORT __declspec(dllexport)

/* A scripted HTTP transport, not a reimplementation of the production state
 * machine. Production C builds JSON, consumes events and interprets statuses. */
typedef int (*http_callback_t)(const char *,const char *,int,int *,char *,unsigned);
static http_callback_t callback;
static int64_t now_us;
static unsigned live_clients;
struct host_http_client {
    esp_http_client_config_t config;
    char *url,*body;
    int method,status;
};
EXPORT void vision_host_init(http_callback_t value) { callback=value;now_us=0; }
EXPORT void vision_host_free(void *pointer) { free(pointer); }
EXPORT unsigned vision_host_live_clients(void) { return live_clients; }
EXPORT void vision_host_advance_ms(unsigned ms) { now_us+=(int64_t)ms*1000; }
EXPORT int host_vision_set_url(const char *url) { return mcp_vision_set_url(url); }
EXPORT void host_vision_get_url(char *out,size_t size) { mcp_vision_get_url(out,size); }
EXPORT char *host_vision_answer(const char *question,mcp_probe_guard_t guard,void *context,bool *failed) {
    return mcp_vision_answer(question,guard,context,failed);
}
int64_t esp_timer_get_time(void) { return now_us; }
void vTaskDelay(unsigned ms) { vision_host_advance_ms(ms); }
void vision_host_log(const char *tag,const char *format,...) { (void)tag;(void)format; }
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) {
    struct host_http_client *client=calloc(1,sizeof(*client));
    if(!client)return NULL;
    client->config=*config;client->url=strdup(config->url);++live_clients;return client;
}
esp_err_t esp_http_client_set_method(esp_http_client_handle_t client,int method) { client->method=method;return ESP_OK; }
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client,const char *key,const char *value) {
    (void)client;(void)key;(void)value;return ESP_OK;
}
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client,const char *body,int size) {
    client->body=malloc((size_t)size+1);
    if(!client->body)return ESP_FAIL;
    memcpy(client->body,body,(size_t)size);client->body[size]=0;return ESP_OK;
}
esp_err_t esp_http_client_perform(esp_http_client_handle_t client) {
    const unsigned capacity=32768;
    char *buffer=malloc(capacity);
    if(!buffer)return ESP_FAIL;
    int length=callback(client->url,client->body,client->config.timeout_ms,&client->status,buffer,capacity);
    esp_err_t result=ESP_OK;
    if(length<0 || (unsigned)length>capacity)result=ESP_FAIL;
    else for(int offset=0;offset<length && result==ESP_OK;) {
        int chunk=length-offset>211?211:length-offset;
        esp_http_client_event_t event={.event_id=HTTP_EVENT_ON_DATA,.user_data=client->config.user_data,
                                      .data=buffer+offset,.data_len=chunk};
        result=client->config.event_handler(&event);offset+=chunk;
    }
    free(buffer);return result;
}
int esp_http_client_get_status_code(esp_http_client_handle_t client) { return client->status; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client) {
    free(client->url);free(client->body);free(client);--live_clients;return ESP_OK;
}
