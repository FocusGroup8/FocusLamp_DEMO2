#ifndef VOICE_CONTROL_MODULE_H
#define VOICE_CONTROL_MODULE_H

#include "esp_err.h"

#include "voice_control_module_config.h"
#include "voice_control_module_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

#if (VOICE_CONTROL_MODULE_ENABLE == 1)

    /**
     * @brief 初始化语音控制模块
     *
     * @param config 配置参数，NULL使用默认配置
     * @return esp_err_t
     *         - ESP_OK: 成功
     *         - ESP_ERR_NO_MEM: 内存不足
     *         - ESP_ERR_INVALID_STATE: 已初始化
     */
    esp_err_t voice_control_module_init(const voice_control_module_config_t* config);

    /**
     * @brief 反初始化语音控制模块
     *
     * @return esp_err_t
     *         - ESP_OK: 成功
     */
    esp_err_t voice_control_module_deinit(void);

    /**
     * @brief 启动语音控制服务
     *
     * @return esp_err_t
     *         - ESP_OK: 成功
     *         - ESP_ERR_INVALID_STATE: 未初始化或已运行
     */
    esp_err_t voice_control_module_start(void);

    /**
     * @brief 停止语音控制服务
     *
     * @return esp_err_t
     *         - ESP_OK: 成功
     */
    esp_err_t voice_control_module_stop(void);

    /**
     * @brief 获取当前状态
     *
     * @return voice_control_module_state_t 当前状态
     */
    voice_control_module_state_t voice_control_module_get_state(void);

    /**
     * @brief 注册模块控制器
     *
     * @param controller 控制器指针
     * @return esp_err_t
     *         - ESP_OK: 成功
     *         - ESP_ERR_INVALID_ARG: 参数无效
     */
    esp_err_t voice_control_module_register_controller(const module_controller_t* controller);

    /**
     * @brief 注销模块控制器
     *
     * @param name 控制器名称
     * @return esp_err_t
     *         - ESP_OK: 成功
     *         - ESP_ERR_NOT_FOUND: 未找到
     */
    esp_err_t voice_control_module_unregister_controller(const char* name);

#else

// 禁用时的空实现
static inline esp_err_t voice_control_module_init(const voice_control_module_config_t* config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t voice_control_module_deinit(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t voice_control_module_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t voice_control_module_stop(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline voice_control_module_state_t voice_control_module_get_state(void)
{
    return VOICE_CONTROL_STATE_UNINIT;
}
static inline esp_err_t
voice_control_module_register_controller(const module_controller_t* controller)
{
    (void)controller;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t voice_control_module_unregister_controller(const char* name)
{
    (void)name;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif

#ifdef __cplusplus
}
#endif

#endif // VOICE_CONTROL_MODULE_H
