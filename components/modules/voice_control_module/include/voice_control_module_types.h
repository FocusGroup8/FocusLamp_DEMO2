#ifndef VOICE_CONTROL_MODULE_TYPES_H
#define VOICE_CONTROL_MODULE_TYPES_H

#include <stdbool.h>

#include "esp_err.h"

#include "voice_control_module_config.h"

#if (VOICE_CONTROL_MODULE_ENABLE == 1)
#include "esp_mcp_engine.h"
#else
typedef void esp_mcp_tool_t;
#endif

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 语音控制模块状态
     */
    typedef enum
    {
        VOICE_CONTROL_STATE_UNINIT = 0, // 未初始化
        VOICE_CONTROL_STATE_IDLE,       // 空闲
        VOICE_CONTROL_STATE_CONNECTING, // 连接中
        VOICE_CONTROL_STATE_LISTENING,  // 监听中
        VOICE_CONTROL_STATE_SPEAKING,   // 说话中
        VOICE_CONTROL_STATE_ERROR,      // 错误
    } voice_control_module_state_t;

    /**
     * @brief 模块控制器回调函数类型
     *
     * @param params JSON参数对象
     * @param result 输出结果JSON对象
     * @return esp_err_t 执行结果
     */
    typedef esp_err_t (*module_controller_callback_t)(void* params, void** result);

    /**
     * @brief 模块控制器结构体
     */
    typedef struct
    {
        const char*                  name;        // 控制器名称（如 "lcd", "servo"）
        const char*                  description; // 控制器描述
        esp_mcp_tool_t**             tools;       // MCP工具数组
        int                          tool_count;  // 工具数量
        module_controller_callback_t init_cb;     // 初始化回调（可选）
        module_controller_callback_t deinit_cb;   // 反初始化回调（可选）
    } module_controller_t;

    /**
     * @brief 语音控制模块配置
     */
    typedef struct
    {
        const char* wake_word;            // 唤醒词，默认 "小智小智"
        bool        enable_lcd_control;   // 启用LCD控制
        bool        enable_servo_control; // 启用舵机控制
        bool        enable_radar_control; // 启用雷达控制
        int         audio_sample_rate;    // 音频采样率
        int         audio_channels;       // 音频通道数
    } voice_control_module_config_t;

/**
 * @brief 默认配置宏
 */
#define VOICE_CONTROL_MODULE_DEFAULT_CONFIG() \
    {                                         \
        .wake_word            = "小智小智",   \
        .enable_lcd_control   = true,         \
        .enable_servo_control = false,        \
        .enable_radar_control = false,        \
        .audio_sample_rate    = 16000,        \
        .audio_channels       = 1,            \
    }

#ifdef __cplusplus
}
#endif

#endif // VOICE_CONTROL_MODULE_TYPES_H
