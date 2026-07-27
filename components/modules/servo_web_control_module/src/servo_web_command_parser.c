#include "servo_web_control_module_config.h"

#if (SERVO_WEB_CONTROL_MODULE_ENABLE == 1)

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"

#include "cJSON.h"
#include "servo_web_control_module_types.h"

static const char* TAG __attribute__((unused)) = "SERVO_PARSER";

esp_err_t servo_web_parse_command(const char* json_str, servo_web_command_t* cmd)
{
    cJSON* root = cJSON_Parse(json_str);
    if (!root)
        return ESP_FAIL;

    cJSON* type_item = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type_item))
    {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    if (strcmp(type_item->valuestring, "em3_move") == 0)
    {
        cmd->type = SERVO_CMD_EM3_MOVE;
        cmd->data.single.id =
            cJSON_GetObjectItem(root, "id") ? cJSON_GetObjectItem(root, "id")->valueint : 4;
        cmd->data.single.position =
            cJSON_GetObjectItem(root, "pos") ? cJSON_GetObjectItem(root, "pos")->valueint : 0;
        cmd->data.single.duration_ms =
            cJSON_GetObjectItem(root, "time") ? cJSON_GetObjectItem(root, "time")->valueint : 500;
    }
    else if (strcmp(type_item->valuestring, "lx_move") == 0)
    {
        cmd->type = SERVO_CMD_LX_MOVE_SINGLE;
        cmd->data.single.id =
            cJSON_GetObjectItem(root, "id") ? cJSON_GetObjectItem(root, "id")->valueint : 1;
        cmd->data.single.position =
            cJSON_GetObjectItem(root, "pos") ? cJSON_GetObjectItem(root, "pos")->valueint : 500;
        cmd->data.single.duration_ms =
            cJSON_GetObjectItem(root, "time") ? cJSON_GetObjectItem(root, "time")->valueint : 500;
    }
    else if (strcmp(type_item->valuestring, "lx_group") == 0)
    {
        cmd->type = SERVO_CMD_LX_MOVE_GROUP;
        cmd->data.group.duration_ms =
            cJSON_GetObjectItem(root, "time") ? cJSON_GetObjectItem(root, "time")->valueint : 500;
        cJSON* servos = cJSON_GetObjectItem(root, "servos");
        if (cJSON_IsArray(servos))
        {
            cmd->data.group.count = cJSON_GetArraySize(servos);
            if (cmd->data.group.count > 6)
                cmd->data.group.count = 6;
            for (int i = 0; i < cmd->data.group.count; i++)
            {
                cJSON* item                        = cJSON_GetArrayItem(servos, i);
                cmd->data.group.params[i].id       = cJSON_GetObjectItem(item, "id")->valueint;
                cmd->data.group.params[i].position = cJSON_GetObjectItem(item, "pos")->valueint;
            }
        }
    }
    else if (strcmp(type_item->valuestring, "get_status") == 0 ||
             strcmp(type_item->valuestring, "servo_status") == 0)
    {
        cmd->type = SERVO_CMD_GET_STATUS;
    }
    else if (strcmp(type_item->valuestring, "servo_record") == 0)
    {
        cmd->type = SERVO_CMD_RECORD_TOGGLE;
    }
    else if (strcmp(type_item->valuestring, "servo_play") == 0)
    {
        cmd->type = SERVO_CMD_PLAY;
    }
    else if (strcmp(type_item->valuestring, "servo_stop") == 0)
    {
        cmd->type = SERVO_CMD_STOP;
    }
    else if (strcmp(type_item->valuestring, "servo_slot") == 0)
    {
        cmd->type = SERVO_CMD_SWITCH_SLOT;
        cmd->slot =
            cJSON_GetObjectItem(root, "slot") ? cJSON_GetObjectItem(root, "slot")->valueint : 0;
    }
    else
    {
        cmd->type = SERVO_CMD_UNKNOWN;
    }

    cJSON_Delete(root);
    return ESP_OK;
}

#endif