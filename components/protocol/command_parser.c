/*
 * command_parser.c - Command parser implementation
 */

#include <string.h>
#include "esp_log.h"
#include "command_parser.h"
#include "event_bus.h"
#include "event_def.h"

static const char *TAG = "cmd_parser";

/* ===================== Parse ===================== */
int command_parse(const uint8_t *data, uint8_t len, command_t *cmd)
{
    if (data == NULL || cmd == NULL || len < 2) {
        return -2; /* ERR_INVALID_PARAM */
    }

    /* First 2 bytes are command type (big-endian) */
    cmd->cmd_type   = (command_type_t)((data[0] << 8) | data[1]);
    cmd->params     = NULL;
    cmd->param_len  = 0;

    if (len > 2) {
        cmd->params    = (uint8_t *)data + 2;
        cmd->param_len = len - 2;
    }

    ESP_LOGD(TAG, "Parsed command 0x%04X, param_len=%d", cmd->cmd_type, cmd->param_len);
    return 0; /* ERR_OK */
}

/* ===================== Dispatch ===================== */
int command_dispatch(const command_t *cmd)
{
    if (cmd == NULL) {
        return -2; /* ERR_INVALID_PARAM */
    }

    ESP_LOGI(TAG, "Dispatching command 0x%04X (seq=%d, params=%d bytes)",
             cmd->cmd_type, cmd->sequence, cmd->param_len);

    /* Copy command data into an event payload so it remains valid during
     * synchronous event dispatch. */
    command_event_data_t event_data = {
        .cmd_type  = cmd->cmd_type,
        .sequence  = cmd->sequence,
        .param_len = cmd->param_len,
    };

    uint8_t copy_len = 0;
    if (cmd->params != NULL && cmd->param_len > 0) {
        copy_len = cmd->param_len;
        if (copy_len > COMMAND_EVENT_MAX_PARAM_LEN) {
            copy_len = COMMAND_EVENT_MAX_PARAM_LEN;
            ESP_LOGW(TAG, "Command params truncated from %d to %d bytes",
                     cmd->param_len, COMMAND_EVENT_MAX_PARAM_LEN);
        }
        memcpy(event_data.params, cmd->params, copy_len);
    }

    event_t ev = {
        .type      = EV_COMMAND_RECEIVED,
        .data      = &event_data,
        .data_size = sizeof(event_data),
        .timestamp = event_bus_get_timestamp(),
    };

    event_bus_publish(&ev);

    return 0; /* ERR_OK */
}