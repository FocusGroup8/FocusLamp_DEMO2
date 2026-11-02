#pragma once
#include <stddef.h>
#include "esp_err.h"
enum { HTTP_GET, HTTP_POST };
#define HTTPD_SOCK_ERR_TIMEOUT (-2)
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
typedef struct httpd_req {
    size_t content_len;
    const char *input;
    size_t input_size, consumed, receive_chunk;
    char response[2048], content_type[48], status[48];
    size_t response_size;
    int close_header;
    int fd, nodelay_at_first_send;
    unsigned send_calls;
} httpd_req_t;
typedef struct {
    const char *uri;
    int method;
    esp_err_t (*handler)(httpd_req_t *);
} httpd_uri_t;
int httpd_req_recv(httpd_req_t *req, char *buffer, size_t length);
int httpd_req_to_sockfd(httpd_req_t *req);
esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type);
esp_err_t httpd_resp_set_status(httpd_req_t *req, const char *status);
esp_err_t httpd_resp_set_hdr(httpd_req_t *req, const char *name, const char *value);
esp_err_t httpd_resp_send(httpd_req_t *req, const char *body, int length);
esp_err_t httpd_resp_sendstr(httpd_req_t *req, const char *body);
esp_err_t httpd_resp_send_chunk(httpd_req_t *req, const char *body, int length);
esp_err_t httpd_resp_sendstr_chunk(httpd_req_t *req, const char *body);
esp_err_t httpd_resp_send_err(httpd_req_t *req, int code, const char *message);
