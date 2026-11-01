
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define HTTP_EVENT_ON_DATA 1
#define HTTP_METHOD_POST 1
#define HTTP_METHOD_GET 0
typedef struct { char host[64]; uint16_t port; } focuslamp_endpoint_t;
typedef struct {void *user_data;int event_id;int data_len;char *data;} esp_http_client_event_t;
typedef struct {const char *url;int timeout_ms;esp_err_t (*event_handler)(esp_http_client_event_t*);void *user_data;int method;} esp_http_client_config_t;
typedef struct {esp_http_client_config_t cfg;const char *body;int length;bool content_type;char url[160];} client_t;
typedef client_t *esp_http_client_handle_t;
typedef int cJSON;
static client_t storage;static unsigned inits,cleanups,performs;static int fail,overflow;
static cJSON parsed;
static cJSON *cJSON_Parse(const char *text){assert(!strcmp(text,"{}"));return &parsed;}
static esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg){++inits;memset(&storage,0,sizeof(storage));storage.cfg=*cfg;return &storage;}
static int esp_http_client_cleanup(client_t *c){assert(c==&storage);++cleanups;return 0;}
static int esp_http_client_set_user_data(client_t *c,void *p){c->cfg.user_data=p;return 0;}
static int esp_http_client_set_url(client_t *c,const char *url){snprintf(c->url,sizeof(c->url),"%s",url);return 0;}
static int esp_http_client_set_method(client_t *c,int m){c->cfg.method=m;return 0;}
static int esp_http_client_set_post_field(client_t *c,const char *p,int n){c->body=p;c->length=n;return 0;}
static int esp_http_client_set_header(client_t *c,const char *k,const char *v){c->content_type=true;return 0;}
static int esp_http_client_delete_header(client_t *c,const char *k){c->content_type=false;return 0;}
static int esp_http_client_get_status_code(client_t *c){return 200;}
static int esp_http_client_perform(client_t *c){
 ++performs;assert(c->cfg.timeout_ms==900);assert(c->cfg.user_data);
 if(c->cfg.method==HTTP_METHOD_GET){assert(c->length==0 && !c->content_type);}
 else assert(c->length>0 && c->content_type);
 if(fail)return -1;
 char large[3072]={0};
 esp_http_client_event_t event={.user_data=c->cfg.user_data,.event_id=1,.data=overflow?large:"{}",.data_len=overflow?3072:2};
 return c->cfg.event_handler(&event);
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

int main(void){
 focuslamp_endpoint_t peer={.host="10.23.38.181",.port=80};int status;
 assert(exchange_path(&peer,"",NULL,&status) && status==200);
 assert(inits==1 && cleanups==0 && storage.cfg.user_data==NULL);
 assert(exchange_path(&peer,"/engagement","{\"sample_seq\":1}",&status));
 assert(inits==1 && storage.length==0 && storage.cfg.user_data==NULL);
 assert(exchange_path(&peer,"",NULL,&status));assert(inits==1);
 fail=1;assert(!exchange_path(&peer,"",NULL,&status) && status==0);assert(cleanups==1);
 fail=0;assert(exchange_path(&peer,"",NULL,&status));assert(inits==2);
 strcpy(peer.host,"10.23.38.182");assert(exchange_path(&peer,"",NULL,&status));assert(inits==3 && cleanups==2);
 overflow=1;assert(!exchange_path(&peer,"",NULL,&status));assert(cleanups==3);
 puts("PASS actual exchange_path: reuse, POST/GET reset, failed transport recovery, peer change, overflow, no retained stack pointers");
}
