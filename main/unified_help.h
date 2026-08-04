/*
 * unified_help.h - Unified help system for FocusLamp console
 */

#pragma once
#ifndef __UNIFIED_HELP_H__
#define __UNIFIED_HELP_H__

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HELP_CATEGORY_SYSTEM,
    HELP_CATEGORY_POWER,
    HELP_CATEGORY_LED,
    HELP_CATEGORY_LCD,
    HELP_CATEGORY_TOUCH,
    HELP_CATEGORY_SERVO,
    HELP_CATEGORY_AUDIO,
    HELP_CATEGORY_SENSOR,
    HELP_CATEGORY_COMM,
    HELP_CATEGORY_ARM,
    HELP_CATEGORY_DEBUG,
    HELP_CATEGORY_RADAR,
    HELP_CATEGORY_LVGL,
    HELP_CATEGORY_PARAM,
    HELP_CATEGORY_MOTION,
    HELP_CATEGORY_MAX,
} help_category_t;

typedef struct {
    const char     *command;
    const char     *help;
    help_category_t category;
} help_command_info_t;

typedef struct {
    const char *name;
    const char *description;
    const char *module_help_command;
} help_module_info_t;

esp_err_t unified_help_init(void);

void unified_help_register_command(const char *command, const char *help,
                                   help_category_t category);

void unified_help_register_module(const char *name, const char *description,
                                  const char *module_help_command);

void register_unified_help_commands(void);

#ifdef __cplusplus
}
#endif

#endif /* __UNIFIED_HELP_H__ */
