#pragma once

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_BRIDGE_ENABLE CONFIG_AUDIO_BRIDGE_ENABLE

#if (AUDIO_BRIDGE_ENABLE == 1)
#define AUDIO_BRIDGE_DEFAULT_SAMPLE_RATE CONFIG_AUDIO_BRIDGE_DEFAULT_SAMPLE_RATE
#define AUDIO_BRIDGE_DEFAULT_VOLUME CONFIG_AUDIO_BRIDGE_DEFAULT_VOLUME
#endif

#ifdef __cplusplus
}
#endif
