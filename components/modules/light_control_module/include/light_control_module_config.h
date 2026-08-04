/**
 * @file light_control_module_config.h
 * @brief Light control module configuration header
 *
 * This file maps Kconfig options to C macros for the light control module.
 *
 * @author CottonLin
 * @date 2026-04-28
 * @version 1.0.0
 */

#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @defgroup LightControlConfig Light Control Configuration
 * @brief Configuration macros for light control module
 * @{
 */

/** @brief Enable light control module */
#define LIGHT_CONTROL_ENABLE CONFIG_PROJECT_ENABLE_LIGHT_CONTROL

#if (LIGHT_CONTROL_ENABLE == 1)

#ifdef CONFIG_LIGHT_CONTROL_AUTO_MODE
/** @brief Enable automatic light adjustment mode */
#define LIGHT_CONTROL_AUTO_MODE 1
#else
/** @brief Disable automatic light adjustment mode */
#define LIGHT_CONTROL_AUTO_MODE 0
#endif

#if (LIGHT_CONTROL_AUTO_MODE == 1)

/** @brief Light control task stack size */
#define LIGHT_CONTROL_TASK_STACK_SIZE CONFIG_LIGHT_CONTROL_TASK_STACK_SIZE

/** @brief Light control task priority */
#define LIGHT_CONTROL_TASK_PRIORITY CONFIG_LIGHT_CONTROL_TASK_PRIORITY

/** @brief Light control task interval in milliseconds */
#define LIGHT_CONTROL_TASK_INTERVAL_MS CONFIG_LIGHT_CONTROL_TASK_INTERVAL_MS

#endif

/** @brief Default LED brightness */
#define LIGHT_CONTROL_DEFAULT_BRIGHTNESS CONFIG_LIGHT_CONTROL_DEFAULT_BRIGHTNESS

/** @brief Default warm LED brightness */
#define LIGHT_CONTROL_DEFAULT_WARM CONFIG_LIGHT_CONTROL_DEFAULT_WARM

/** @brief Default cold LED brightness */
#define LIGHT_CONTROL_DEFAULT_COLD CONFIG_LIGHT_CONTROL_DEFAULT_COLD

/** @brief Default LED color red component */
#define LIGHT_CONTROL_DEFAULT_COLOR_R CONFIG_LIGHT_CONTROL_DEFAULT_COLOR_R

/** @brief Default LED color green component */
#define LIGHT_CONTROL_DEFAULT_COLOR_G CONFIG_LIGHT_CONTROL_DEFAULT_COLOR_G

/** @brief Default LED color blue component */
#define LIGHT_CONTROL_DEFAULT_COLOR_B CONFIG_LIGHT_CONTROL_DEFAULT_COLOR_B

#endif

#ifdef __cplusplus
}
#endif
