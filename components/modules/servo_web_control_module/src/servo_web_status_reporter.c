#include "servo_web_control_module_config.h"

#if (SERVO_WEB_CONTROL_MODULE_ENABLE == 1)

#include "esp_log.h"

#include "cJSON.h"
#include "servo_control_module.h"
#include "servo_driver.h"
#include "servo_web_control_module.h"

extern esp_err_t servo_web_send_ws_data(const char* data);

void servo_web_report_status(void)
{
    cJSON* root = cJSON_CreateObject();
    if (!root)
        return;

    cJSON_AddStringToObject(root, "type", "servo_status");
    cJSON* data = cJSON_AddObjectToObject(root, "data");

    // EM3 舵机 (ID 4)
    int16_t em3_pos = servo_em3_read_pos(4);
    cJSON_AddNumberToObject(data, "em3_4", em3_pos);

    // LX 舵机 (ID: 1, 2, 3, 5)
    uint8_t lx_ids[] = {1, 2, 3, 5};
    for (int i = 0; i < 4; i++)
    {
        char key[10];
        snprintf(key, sizeof(key), "lx_%d", lx_ids[i]);
        int16_t pos = servo_lx_read_pos(lx_ids[i]);
        cJSON_AddNumberToObject(data, key, pos);
    }

    servo_control_status_t status  = servo_control_get_status();
    cJSON*                 control = cJSON_AddObjectToObject(data, "control");
    cJSON_AddNumberToObject(control, "state", status.current_state);
    cJSON_AddNumberToObject(control, "slot", status.current_slot);
    cJSON_AddNumberToObject(control, "frame_count", status.frame_count);
    cJSON_AddNumberToObject(control, "slot0_frames", servo_control_get_frame_count(0));
    cJSON_AddNumberToObject(control, "slot1_frames", servo_control_get_frame_count(1));
    cJSON_AddNumberToObject(control, "slot2_frames", servo_control_get_frame_count(2));

    char* json_str = cJSON_PrintUnformatted(root);
    if (json_str)
    {
        servo_web_send_ws_data(json_str);
        free(json_str);
    }
    cJSON_Delete(root);
}

#endif