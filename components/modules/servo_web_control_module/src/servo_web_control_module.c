#include "servo_web_control_module_config.h"

#if (SERVO_WEB_CONTROL_MODULE_ENABLE == 1)

#include <string.h>

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "servo_web_control_module.h"

static const char* TAG = "SERVO_WEB_MOD";

static servo_web_control_state_t  s_state = SERVO_WEB_CONTROL_STATE_UNINIT;
static servo_web_control_config_t s_config;
static TaskHandle_t               s_serial_bridge_task_handle = NULL;
static bool                       s_run_tasks                 = false;

// 声明内部函数（实现在其它文件中）
extern esp_err_t servo_web_start_http_server(const servo_web_control_config_t* config);
extern esp_err_t servo_web_stop_http_server(void);
extern esp_err_t servo_web_parse_command(const char* json_str, servo_web_command_t* cmd);
extern void      servo_web_execute_command(const servo_web_command_t* cmd);

static void serial_bridge_task(void* pvParameters)
{
    char buf[256];
    ESP_LOGI(TAG, "Serial bridge task started");
    while (s_run_tasks)
    {
        // 注意：在实际项目中，gets 应当替换为更安全的串口读取方式
        // 这里保留逻辑作为占位符
        if (fgets(buf, sizeof(buf), stdin) != NULL)
        {
            servo_web_command_t cmd;
            if (servo_web_parse_command(buf, &cmd) == ESP_OK)
            {
                servo_web_execute_command(&cmd);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelete(NULL);
}

extern void servo_web_report_status(void);

static void status_report_task(void* pvParameters)
{
    ESP_LOGI(TAG, "Status report task started");
    while (s_run_tasks)
    {
        if (s_state == SERVO_WEB_CONTROL_STATE_RUNNING)
        {
            servo_web_report_status();
        }
        vTaskDelay(pdMS_TO_TICKS(2000)); // 每2秒上报一次状态
    }
    vTaskDelete(NULL);
}

esp_err_t servo_web_control_module_init(const servo_web_control_config_t* config)
{
    if (s_state != SERVO_WEB_CONTROL_STATE_UNINIT)
    {
        ESP_LOGW(TAG, "Module already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (config)
    {
        memcpy(&s_config, config, sizeof(servo_web_control_config_t));
    }
    else
    {
        servo_web_control_config_t default_cfg = SERVO_WEB_CONTROL_DEFAULT_CONFIG();
        memcpy(&s_config, &default_cfg, sizeof(servo_web_control_config_t));
    }

    ESP_LOGI(TAG, "Initializing Web Control Module (Port: %d, Endpoint: %s)...", s_config.ws_port,
             s_config.ws_endpoint);

    s_state = SERVO_WEB_CONTROL_STATE_IDLE;
    return ESP_OK;
}

esp_err_t servo_web_control_module_deinit(void)
{
    if (s_state == SERVO_WEB_CONTROL_STATE_RUNNING)
    {
        servo_web_control_module_stop();
    }
    s_state = SERVO_WEB_CONTROL_STATE_UNINIT;
    return ESP_OK;
}

esp_err_t servo_web_control_module_start(void)
{
    if (s_state != SERVO_WEB_CONTROL_STATE_IDLE)
    {
        ESP_LOGE(TAG, "Invalid state for start: %d", s_state);
        return ESP_ERR_INVALID_STATE;
    }

    s_run_tasks = true;

    // 1. 启动 WebSocket 服务器
    esp_err_t ret = servo_web_start_http_server(&s_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        s_run_tasks = false;
        return ret;
    }

    // 2. 启动串口监听任务
    if (s_config.enable_serial_bridge)
    {
        xTaskCreate(serial_bridge_task, "serial_bridge", 4096, NULL, 5,
                    &s_serial_bridge_task_handle);
    }

    // 3. 启动状态定时上报任务
    xTaskCreate(status_report_task, "status_report", 4096, NULL, 4, NULL);

    s_state = SERVO_WEB_CONTROL_STATE_RUNNING;
    ESP_LOGI(TAG, "Web Control Module started successfully");
    return ESP_OK;
}

esp_err_t servo_web_control_module_stop(void)
{
    if (s_state != SERVO_WEB_CONTROL_STATE_RUNNING)
    {
        return ESP_OK;
    }

    s_run_tasks = false;
    servo_web_stop_http_server();

    // 串口任务会自动退出（基于 s_run_tasks）
    s_serial_bridge_task_handle = NULL;

    s_state = SERVO_WEB_CONTROL_STATE_IDLE;
    ESP_LOGI(TAG, "Web Control Module stopped");
    return ESP_OK;
}

servo_web_control_state_t servo_web_control_module_get_state(void)
{
    return s_state;
}

#endif