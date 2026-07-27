#ifndef VOICE_CONTROL_MODULE_CONFIG_H
#define VOICE_CONTROL_MODULE_CONFIG_H

#include "sdkconfig.h"

#ifndef __has_include
#define __has_include(x) 0
#endif

#ifdef __cplusplus
extern "C"
{
#endif

#if __has_include("esp_mcp_engine.h") && __has_include("esp_afe_sr_iface.h") && \
    __has_include("esp_mn_iface.h")
#define VOICE_CONTROL_MODULE_HAS_DEPENDENCIES 1
#else
#define VOICE_CONTROL_MODULE_HAS_DEPENDENCIES 0
#endif

/* Voice control needs ESP-MCP and ESP-SR components, so only enable it when
 * the project explicitly requests it and the external headers are available. */
#ifndef VOICE_CONTROL_MODULE_ENABLE
#if defined(CONFIG_PROJECT_ENABLE_VOICE_CONTROL_MODULE) && \
    (CONFIG_PROJECT_ENABLE_VOICE_CONTROL_MODULE == 1) &&   \
    (VOICE_CONTROL_MODULE_HAS_DEPENDENCIES == 1)
#define VOICE_CONTROL_MODULE_ENABLE 1
#else
#define VOICE_CONTROL_MODULE_ENABLE 0
#endif
#endif

#if (VOICE_CONTROL_MODULE_ENABLE == 1)

/* Default configuration for dowm project */
#ifndef VOICE_CONTROL_WAKE_WORD
#define VOICE_CONTROL_WAKE_WORD "小智小智"
#endif

#ifndef VOICE_CONTROL_ENABLE_LCD
#define VOICE_CONTROL_ENABLE_LCD 0
#endif

#ifndef VOICE_CONTROL_ENABLE_SERVO
#define VOICE_CONTROL_ENABLE_SERVO 0
#endif

#ifndef VOICE_CONTROL_ENABLE_RADAR
#define VOICE_CONTROL_ENABLE_RADAR 0
#endif

#ifndef VOICE_CONTROL_SAMPLE_RATE
#define VOICE_CONTROL_SAMPLE_RATE 16000
#endif

#ifndef VOICE_CONTROL_CHANNELS
#define VOICE_CONTROL_CHANNELS 1
#endif

#ifndef VOICE_CONTROL_TASK_STACK_SIZE
#define VOICE_CONTROL_TASK_STACK_SIZE 8192
#endif

#ifndef VOICE_CONTROL_TASK_PRIORITY
#define VOICE_CONTROL_TASK_PRIORITY 10
#endif

#endif

#ifdef __cplusplus
}
#endif

#endif // VOICE_CONTROL_MODULE_CONFIG_H
