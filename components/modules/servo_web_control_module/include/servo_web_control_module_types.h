#ifndef SERVO_WEB_CONTROL_MODULE_TYPES_H
#define SERVO_WEB_CONTROL_MODULE_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * @brief 模块状态
 */
typedef enum
{
    SERVO_WEB_CONTROL_STATE_UNINIT = 0,
    SERVO_WEB_CONTROL_STATE_IDLE,
    SERVO_WEB_CONTROL_STATE_RUNNING,
    SERVO_WEB_CONTROL_STATE_ERROR
} servo_web_control_state_t;

/**
 * @brief 命令类型
 */
typedef enum
{
    SERVO_CMD_EM3_MOVE,       // EM3 舵机移动
    SERVO_CMD_LX_MOVE_SINGLE, // LX 舵机单体移动
    SERVO_CMD_LX_MOVE_GROUP,  // LX 舵机群组移动
    SERVO_CMD_GET_STATUS,     // 获取当前状态
    SERVO_CMD_RECORD_TOGGLE,  // 开始/停止录制
    SERVO_CMD_PLAY,           // 播放录制动作
    SERVO_CMD_STOP,           // 停止播放/录制
    SERVO_CMD_SWITCH_SLOT,    // 切换录制槽位
    SERVO_CMD_UNKNOWN
} servo_web_cmd_type_t;

/**
 * @brief 舵机控制参数结构体
 */
typedef struct
{
    uint8_t  id;
    uint16_t position;
    uint16_t duration_ms;
} servo_web_move_params_t;

/**
 * @brief 网页发送的控制命令包
 */
typedef struct
{
    servo_web_cmd_type_t type;
    uint8_t              slot;
    union
    {
        servo_web_move_params_t single;
        struct
        {
            uint16_t                duration_ms;
            uint8_t                 count;
            servo_web_move_params_t params[6]; // 最多支持6个LX舵机
        } group;
    } data;
} servo_web_command_t;

/**
 * @brief Web 控制模块配置
 */
typedef struct
{
    uint16_t    ws_port;
    const char* ws_endpoint;
    bool        enable_serial_bridge;
} servo_web_control_config_t;

#endif // SERVO_WEB_CONTROL_MODULE_TYPES_H