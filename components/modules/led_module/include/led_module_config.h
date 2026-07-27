#ifndef LED_MODULE_CONFIG_H
#define LED_MODULE_CONFIG_H

#include "project_config.h"
#include "sdkconfig.h"

#ifdef CONFIG_PROJECT_LED_MODULE_ENABLE
#define LED_MODULE_ENABLE 1
#else
#define LED_MODULE_ENABLE 0
#endif

#if (LED_MODULE_ENABLE == 1)

#include "led_config.h"

#endif

#endif
