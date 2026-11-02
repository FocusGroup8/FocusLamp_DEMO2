#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef void *httpd_handle_t;
enum {HTTP_GET, HTTP_POST};
enum {HTTPD_WS_TYPE_TEXT=1, HTTPD_WS_TYPE_BINARY=2, HTTPD_WS_TYPE_CLOSE=8,
      HTTPD_WS_TYPE_PING=9, HTTPD_WS_TYPE_PONG=10};
enum {HTTPD_WS_CLIENT_INVALID, HTTPD_WS_CLIENT_WEBSOCKET};
enum {HTTPD_400_BAD_REQUEST=400};
typedef struct {
    uint8_t *payload;
    size_t len;
    uint8_t type;
    bool final;
} httpd_ws_frame_t;
typedef struct httpd_req {
    size_t content_len;
    httpd_handle_t handle;
    int method, fd;
    const char *uri;
    httpd_ws_frame_t incoming;
} httpd_req_t;
typedef struct {
    const char *uri;
    int method;
    esp_err_t (*handler)(httpd_req_t *);
    void *user_ctx;
    bool is_websocket, handle_ws_control_frames;
} httpd_uri_t;
typedef struct {
    int stack_size, send_wait_timeout, recv_wait_timeout, server_port;
    int max_uri_handlers, max_open_sockets;
    bool lru_purge_enable;
    void (*close_fn)(httpd_handle_t, int);
    esp_err_t (*open_fn)(httpd_handle_t, int);
} httpd_config_t;
#define HTTPD_DEFAULT_CONFIG() ((httpd_config_t){0})
int httpd_req_to_sockfd(httpd_req_t *req);
int httpd_ws_get_fd_info(httpd_handle_t handle, int fd);
esp_err_t httpd_ws_recv_frame(httpd_req_t *req, httpd_ws_frame_t *frame, size_t max_len);
esp_err_t httpd_ws_send_frame_async(httpd_handle_t handle, int fd, const httpd_ws_frame_t *frame);
esp_err_t httpd_resp_send_err(httpd_req_t *req, int code, const char *message);
esp_err_t httpd_resp_sendstr(httpd_req_t *req, const char *text);
esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type);
esp_err_t httpd_sess_trigger_close(httpd_handle_t handle, int fd);
esp_err_t httpd_sess_update_lru_counter(httpd_handle_t handle, int fd);
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri);
esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config);
esp_err_t httpd_stop(httpd_handle_t handle);
