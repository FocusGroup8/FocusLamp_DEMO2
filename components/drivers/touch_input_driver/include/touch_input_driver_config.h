#ifndef TOUCH_INPUT_DRIVER_CONFIG_H
#define TOUCH_INPUT_DRIVER_CONFIG_H

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C"
{
#endif

#ifdef CONFIG_PROJECT_ENABLE_TOUCH_INPUT
#define TOUCH_INPUT_DRIVER_ENABLE 1
#else
#define TOUCH_INPUT_DRIVER_ENABLE 0
#endif

#if (TOUCH_INPUT_DRIVER_ENABLE == 1)

#define TOUCH_INPUT_A_PIN CONFIG_TOUCH_INPUT_A_PIN
#define TOUCH_INPUT_B_PIN CONFIG_TOUCH_INPUT_B_PIN
#define TOUCH_INPUT_C_PIN CONFIG_TOUCH_INPUT_C_PIN

#endif

#ifdef __cplusplus
}
#endif

#endif