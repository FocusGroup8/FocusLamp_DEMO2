#include "servo_web_control_module_config.h"

#if (SERVO_WEB_CONTROL_MODULE_ENABLE == 1)

#include "esp_log.h"

#include "servo_control_module.h"
#include "servo_driver.h"
#include "servo_web_control_module_types.h"

static const char* TAG = "SERVO_EXECUTOR";

extern void servo_web_report_status(void);

void servo_web_execute_command(const servo_web_command_t* cmd)
{
    ESP_LOGI(TAG, "正在执行指令: 类型=%d", cmd->type);
    switch (cmd->type)
    {
    case SERVO_CMD_EM3_MOVE:
        ESP_LOGI(TAG, "EM3 Move: ID=%d, Pos=%d", cmd->data.single.id, cmd->data.single.position);
        servo_em3_move(cmd->data.single.id, cmd->data.single.position,
                       cmd->data.single.duration_ms);
        break;

    case SERVO_CMD_LX_MOVE_SINGLE:
        ESP_LOGI(TAG, "LX Move: ID=%d, Pos=%d", cmd->data.single.id, cmd->data.single.position);
        servo_lx_move(cmd->data.single.id, (int16_t)cmd->data.single.position,
                      cmd->data.single.duration_ms);
        break;

    case SERVO_CMD_LX_MOVE_GROUP:
    {
        uint8_t ids[6];
        int16_t positions[6];
        for (int i = 0; i < cmd->data.group.count; i++)
        {
            ids[i]       = cmd->data.group.params[i].id;
            positions[i] = (int16_t)cmd->data.group.params[i].position;
        }
        ESP_LOGI(TAG, "LX Group Move: Count=%d", cmd->data.group.count);
        servo_lx_move_group(ids, positions, cmd->data.group.count, cmd->data.group.duration_ms);
        break;
    }

    case SERVO_CMD_GET_STATUS:
        ESP_LOGI(TAG, "收到状态获取请求");
        servo_web_report_status();
        break;

    case SERVO_CMD_RECORD_TOGGLE:
    {
        servo_control_status_t status = servo_control_get_status();
        if (status.current_state == SERVO_CONTROL_STATE_RECORDING)
        {
            ESP_LOGI(TAG, "停止录制");
            servo_control_stop_recording();
        }
        else
        {
            ESP_LOGI(TAG, "开始录制");
            servo_control_start_recording();
        }
        servo_web_report_status();
        break;
    }

    case SERVO_CMD_PLAY:
        ESP_LOGI(TAG, "开始播放");
        servo_control_start_playback();
        servo_web_report_status();
        break;

    case SERVO_CMD_STOP:
    {
        servo_control_status_t status = servo_control_get_status();
        ESP_LOGI(TAG, "停止播放/录制");
        if (status.current_state == SERVO_CONTROL_STATE_RECORDING)
        {
            servo_control_stop_recording();
        }
        servo_control_stop_playback();
        servo_web_report_status();
        break;
    }

    case SERVO_CMD_SWITCH_SLOT:
        ESP_LOGI(TAG, "切换槽位: %d", cmd->slot);
        servo_control_switch_slot(cmd->slot);
        servo_web_report_status();
        break;

    default:
        ESP_LOGW(TAG, "Unknown command type");
        break;
    }
}

#endif