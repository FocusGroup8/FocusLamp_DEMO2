#ifndef SMART_VOICE_CONSOLE_H
#define SMART_VOICE_CONSOLE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Register Smart Voice Control Module console commands
     *
     * Registers the following commands:
     * - smart_voice_status: Show module status
     * - smart_voice_init: Initialize module
     * - smart_voice_start: Start module
     * - smart_voice_stop: Stop module
     * - smart_voice_mode: Get/Set operation mode
     * - smart_voice_cmds: List all loaded commands
     * - smart_voice_find: Find command by keyword
     * - smart_voice_reload: Reload config file
     * - smart_voice_test_event: Send test events
     *
     * @return esp_err_t
     */
    esp_err_t smart_voice_register_console_commands(void);

#ifdef __cplusplus
}
#endif

#endif // SMART_VOICE_CONSOLE_H
