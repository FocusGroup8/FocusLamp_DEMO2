#pragma once
#include <stdbool.h>
#include "esp_err.h"
typedef struct { int event_id; void *user_data; const char *data; int data_len; } esp_http_client_event_t;
typedef struct {
    const char *url;
    int timeout_ms;
    esp_err_t (*event_handler)(esp_http_client_event_t *);
    void *user_data;
    bool disable_auto_redirect;
} esp_http_client_config_t;
typedef struct host_http_client *esp_http_client_handle_t;
#define HTTP_EVENT_ON_DATA 1
#define HTTP_METHOD_POST 1
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *);
esp_err_t esp_http_client_set_method(esp_http_client_handle_t,int);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t,const char *,const char *);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t,const char *,int);
esp_err_t esp_http_client_perform(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t);
