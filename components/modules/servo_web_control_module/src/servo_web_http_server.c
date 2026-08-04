#include "servo_web_control_module_config.h"

#if (SERVO_WEB_CONTROL_MODULE_ENABLE == 1)

#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "servo_web_control_module.h"

static const char*    TAG      = "SERVO_HTTP";
static httpd_handle_t s_server = NULL;

extern esp_err_t servo_web_parse_command(const char* json_str, servo_web_command_t* cmd);
extern void      servo_web_execute_command(const servo_web_command_t* cmd);

static esp_err_t ws_handler(httpd_req_t* req)
{
    if (req->method == HTTP_GET)
    {
        ESP_LOGI(TAG, "Handshake done, the new connection was opened");
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    uint8_t*         buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK)
        return ret;

    if (ws_pkt.len)
    {
        buf = calloc(1, ws_pkt.len + 1);
        if (buf == NULL)
            return ESP_ERR_NO_MEM;
        ws_pkt.payload = buf;
        ret            = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret == ESP_OK)
        {
            ESP_LOGI(TAG, "收到 WebSocket 数据: %s", (char*)ws_pkt.payload);
            servo_web_command_t cmd;
            if (servo_web_parse_command((const char*)ws_pkt.payload, &cmd) == ESP_OK)
            {
                servo_web_execute_command(&cmd);
            }
        }
        free(buf);
    }
    return ret;
}

esp_err_t servo_web_start_http_server(const servo_web_control_config_t* config)
{
    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.server_port    = config->ws_port;
    http_cfg.ctrl_port      = config->ws_port + 1;

    static const httpd_uri_t ws = {.uri          = "/ws",
                                   .method       = HTTP_GET,
                                   .handler      = ws_handler,
                                   .user_ctx     = NULL,
                                   .is_websocket = true};

    if (httpd_start(&s_server, &http_cfg) == ESP_OK)
    {
        httpd_register_uri_handler(s_server, &ws);
        ESP_LOGI(TAG, "WebSocket 服务器启动成功! 端口: %d, 路径: /ws", http_cfg.server_port);
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t servo_web_stop_http_server(void)
{
    if (s_server)
    {
        httpd_stop(s_server);
        s_server = NULL;
    }
    return ESP_OK;
}

esp_err_t servo_web_send_ws_data(const char* data)
{
    if (!s_server)
        return ESP_ERR_INVALID_STATE;

    size_t clients = 8;
    int    client_fds[8];
    if (httpd_get_client_list(s_server, &clients, client_fds) != ESP_OK)
        return ESP_FAIL;

    for (size_t i = 0; i < clients; i++)
    {
        if (httpd_ws_get_fd_info(s_server, client_fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET)
        {
            httpd_ws_frame_t ws_pkt;
            memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
            ws_pkt.payload = (uint8_t*)data;
            ws_pkt.len     = strlen(data);
            ws_pkt.type    = HTTPD_WS_TYPE_TEXT;
            httpd_ws_send_frame_async(s_server, client_fds[i], &ws_pkt);
        }
    }
    return ESP_OK;
}

#endif