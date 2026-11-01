from pathlib import Path
p=Path('controller/components/websocket_manager/src/websocket_manager.c');s=p.read_text(encoding='utf8');needle='    httpd_config_t config = HTTPD_DEFAULT_CONFIG();';assert s.count(needle)==1;s=s.replace(needle,needle+'\n    /* Mode/engagement JSON plus HTTPD parsing and logging share this task. */\n    config.stack_size = 8192;');p.write_text(s,encoding='utf8')
p=Path('controller/main/mode_http_api.c');s=p.read_text(encoding='utf8').replace('#include "esp_timer.h"','#include "esp_timer.h"\n#include "freertos/FreeRTOS.h"\n#include "freertos/task.h"');s=s.replace('"{\\"uptime_ms\\":%lu,','"{\\"http_stack_free_bytes\\":%u,\\"uptime_ms\\":%lu,');s=s.replace('        (unsigned long)state.uptime_ms,','        (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned long)state.uptime_ms,')
marker='static esp_err_t engagement_handler(httpd_req_t *req)';i=s.index(marker)
s=s[:i]+'''/* Compact reply: the head only consumes the authoritative clock. Avoid
 * nesting the full diagnostics payload under the engagement request body. */
static esp_err_t engagement_response(httpd_req_t *req)
{
    controller_modes_status_t s;
    if(controller_modes_get_status(&s)!=ESP_OK) return httpd_resp_send_err(req,HTTPD_503_SERVICE_UNAVAILABLE,"modes not ready");
    char body[512];
    int n=snprintf(body,sizeof(body),
        "{\\"ok\\":true,\\"mode\\":\\"%s\\",\\"uptime_ms\\":%lu,\\"controller_boot_id\\":%lu,"
        "\\"session_id\\":%lu,\\"seq\\":%lu,\\"engagement_token\\":%lu,\\"duration_ms\\":%lu,"
        "\\"remaining_ms\\":%lu,\\"rate_half\\":%d,\\"pause_reason\\":\\"%s\\",\\"paused\\":%s,\\"completed\\":%s}",
        controller_modes_name(s.mode),(unsigned long)s.uptime_ms,(unsigned long)s.controller_boot_id,
        (unsigned long)s.session_id,(unsigned long)s.seq,(unsigned long)s.engagement_token,
        (unsigned long)s.duration_ms,(unsigned long)s.remaining_ms,s.rate_half,
        controller_modes_pause_name(s.pause_reason),s.paused?"true":"false",s.completed?"true":"false");
    if(n<0 || n>=(int)sizeof(body))return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"response overflow");
    static int64_t report_at;
    int64_t now=esp_timer_get_time();
    if(now-report_at>=10000000){
        report_at=now;
        ESP_LOGI(TAG,"[MODE_HTTP] stack_free_bytes=%u session=%lu reason=%s",(unsigned)uxTaskGetStackHighWaterMark(NULL),
            (unsigned long)s.session_id,controller_modes_pause_name(s.pause_reason));
    }
    return httpd_resp_send(req,body,n);
}
'''+s[i:]
start=s.index('static esp_err_t engagement_handler');a=s[:start];b=s[start:].replace('return mode_status_response(req);','return engagement_response(req);',1);s=a+b;p.write_text(s,encoding='utf8')
p=Path('controller/main/device_info_api.c');s=p.read_text(encoding='utf8').replace('#include "esp_log.h"','#include "esp_log.h"\n#include "esp_system.h"\n#include "esp_timer.h"');s=s.replace('!cJSON_AddNumberToObject(root, "http_port", CONFIG_WS_MANAGER_SERVER_PORT))','''!cJSON_AddNumberToObject(root, "http_port", CONFIG_WS_MANAGER_SERVER_PORT) ||
        !cJSON_AddNumberToObject(root, "reset_reason", esp_reset_reason()) ||
        !cJSON_AddNumberToObject(root, "uptime_ms", esp_timer_get_time()/1000))''');p.write_text(s,encoding='utf8')
p=Path('controller/main/hello_world_main.c');s=p.read_text(encoding='utf8').replace('#include "esp_log.h"','#include "esp_log.h"\n#include "esp_system.h"');s=s.replace('    ESP_LOGI(TAG, "=== Smart Voice Assistant for WT01P4C5-S1 ===");','    ESP_LOGI(TAG, "=== Smart Voice Assistant for WT01P4C5-S1 ===");\n    ESP_LOGI(TAG, "[RESET_DIAG] reset_reason=%d", (int)esp_reset_reason());');p.write_text(s,encoding='utf8')
