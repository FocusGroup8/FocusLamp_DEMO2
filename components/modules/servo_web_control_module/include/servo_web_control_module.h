#ifndef SERVO_WEB_CONTROL_MODULE_H
#define SERVO_WEB_CONTROL_MODULE_H

#include "esp_err.h"

#include "servo_web_control_module_config.h"
#include "servo_web_control_module_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

#if (SERVO_WEB_CONTROL_MODULE_ENABLE == 1)

#define SERVO_WEB_CONTROL_DEFAULT_CONFIG()                          \
    {.ws_port              = SERVO_WEB_CONTROL_DEFAULT_WS_PORT,     \
     .ws_endpoint          = SERVO_WEB_CONTROL_DEFAULT_WS_ENDPOINT, \
     .enable_serial_bridge = SERVO_WEB_CONTROL_DEFAULT_SERIAL_BRIDGE}

    /**
     * @brief 初始化 Web 控制模块
     *
     * @param config 配置参数，NULL使用默认配置
     * @return esp_err_t
     */
    esp_err_t servo_web_control_module_init(const servo_web_control_config_t* config);

    /**
     * @brief 反初始化 Web 控制模块
     */
    esp_err_t servo_web_control_module_deinit(void);

    /**
     * @brief 启动 Web 控制服务
     */
    esp_err_t servo_web_control_module_start(void);

    /**
     * @brief 停止 Web 控制服务
     */
    esp_err_t servo_web_control_module_stop(void);

    /**
     * @brief 获取当前模块状态
     */
    servo_web_control_state_t servo_web_control_module_get_state(void);

#else

// 禁用时的空实现
static inline esp_err_t servo_web_control_module_init(const servo_web_control_config_t* config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t servo_web_control_module_deinit(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t servo_web_control_module_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t servo_web_control_module_stop(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline servo_web_control_state_t servo_web_control_module_get_state(void)
{
    return SERVO_WEB_CONTROL_STATE_UNINIT;
}

#endif

#ifdef __cplusplus
}
#endif

#endif // SERVO_WEB_CONTROL_MODULE_H