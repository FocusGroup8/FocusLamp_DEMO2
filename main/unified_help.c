/*
 * unified_help.c - Unified help system for FocusLamp console
 */

#include "unified_help.h"

#include <stdio.h>
#include <string.h>

#include "esp_console.h"
#include "esp_log.h"

#include "argtable3/argtable3.h"

#define MAX_COMMANDS 100
#define MAX_MODULES  20

static const char *TAG = "UNIFIED_HELP";

static help_command_info_t s_commands[MAX_COMMANDS];
static int                 s_command_count = 0;

static help_module_info_t s_modules[MAX_MODULES];
static int                s_module_count = 0;

static const char *s_category_names[] = {
    [HELP_CATEGORY_SYSTEM] = "System",
    [HELP_CATEGORY_POWER]  = "Power",
    [HELP_CATEGORY_LED]    = "LED",
    [HELP_CATEGORY_LCD]    = "LCD",
    [HELP_CATEGORY_TOUCH]  = "Touch",
    [HELP_CATEGORY_SERVO]  = "Servo",
    [HELP_CATEGORY_AUDIO]  = "Audio",
    [HELP_CATEGORY_SENSOR] = "Sensor",
    [HELP_CATEGORY_COMM]   = "Communication",
    [HELP_CATEGORY_ARM]    = "Arm",
    [HELP_CATEGORY_DEBUG]  = "Debug",
    [HELP_CATEGORY_RADAR]  = "Radar",
    [HELP_CATEGORY_LVGL]   = "LVGL",
    [HELP_CATEGORY_PARAM]  = "Parameters",
    [HELP_CATEGORY_MOTION] = "Motion",
};

static const char *s_category_colors[] = {
    [HELP_CATEGORY_SYSTEM] = "\033[36m",
    [HELP_CATEGORY_POWER]  = "\033[33m",
    [HELP_CATEGORY_LED]    = "\033[34m",
    [HELP_CATEGORY_LCD]    = "\033[35m",
    [HELP_CATEGORY_TOUCH]  = "\033[38;5;208m",
    [HELP_CATEGORY_SERVO]  = "\033[32m",
    [HELP_CATEGORY_AUDIO]  = "\033[38;5;93m",
    [HELP_CATEGORY_SENSOR] = "\033[38;5;46m",
    [HELP_CATEGORY_COMM]   = "\033[38;5;226m",
    [HELP_CATEGORY_ARM]    = "\033[38;5;129m",
    [HELP_CATEGORY_DEBUG]  = "\033[31m",
    [HELP_CATEGORY_RADAR]  = "\033[38;5;202m",
    [HELP_CATEGORY_LVGL]   = "\033[38;5;45m",
    [HELP_CATEGORY_PARAM]  = "\033[38;5;82m",
    [HELP_CATEGORY_MOTION] = "\033[38;5;44m",
};

esp_err_t unified_help_init(void)
{
    s_command_count = 0;
    s_module_count  = 0;
    memset(s_commands, 0, sizeof(s_commands));
    memset(s_modules, 0, sizeof(s_modules));

    ESP_LOGI(TAG, "Unified help system initialized");
    return ESP_OK;
}

void unified_help_register_command(const char *command, const char *help, help_category_t category)
{
    if (s_command_count >= MAX_COMMANDS) {
        ESP_LOGW(TAG, "Command registry full, cannot register: %s", command);
        return;
    }

    s_commands[s_command_count].command  = command;
    s_commands[s_command_count].help     = help;
    s_commands[s_command_count].category = category;
    s_command_count++;
}

void unified_help_register_module(const char *name, const char *description,
                                  const char *module_help_command)
{
    if (s_module_count >= MAX_MODULES) {
        ESP_LOGW(TAG, "Module registry full, cannot register: %s", name);
        return;
    }

    s_modules[s_module_count].name                = name;
    s_modules[s_module_count].description         = description;
    s_modules[s_module_count].module_help_command = module_help_command;
    s_module_count++;
}

static void print_all_commands(void)
{
    printf("\n\033[1;37m========================================\033[0m\n");
    printf("\033[1;37m        Available Commands\033[0m\n");
    printf("\033[1;37m========================================\033[0m\n\n");

    for (int cat = 0; cat < HELP_CATEGORY_MAX; cat++) {
        bool has_commands = false;

        for (int i = 0; i < s_command_count; i++) {
            if (s_commands[i].category == cat) {
                if (!has_commands) {
                    printf("\033[1m%s[%s]\033[0m\n", s_category_colors[cat], s_category_names[cat]);
                    has_commands = true;
                }
                printf("  \033[0m%-30s %s\n", s_commands[i].command, s_commands[i].help);
            }
        }

        if (has_commands) {
            printf("\n");
        }
    }

    printf("\033[1;37m----------------------------------------\033[0m\n");
    printf("Usage: help <command> for detailed help\n");
    printf("       help <module> for module-specific help\n");
    printf("       help search <keyword> to search commands\n");
    printf("\033[1;37m========================================\033[0m\n\n");
}

static void print_module_help(const char *module_name)
{
    for (int i = 0; i < s_module_count; i++) {
        if (strcasecmp(s_modules[i].name, module_name) == 0) {
            printf("\n\033[1;37m========================================\033[0m\n");
            printf("\033[1;37m  Module: %s\033[0m\n", s_modules[i].name);
            printf("\033[1;37m========================================\033[0m\n");
            printf("\n%s\n\n", s_modules[i].description);

            help_category_t category = HELP_CATEGORY_SYSTEM;

            if (strcasecmp(module_name, "power") == 0)
                category = HELP_CATEGORY_POWER;
            else if (strcasecmp(module_name, "led") == 0)
                category = HELP_CATEGORY_LED;
            else if (strcasecmp(module_name, "lcd") == 0)
                category = HELP_CATEGORY_LCD;
            else if (strcasecmp(module_name, "touch") == 0)
                category = HELP_CATEGORY_TOUCH;
            else if (strcasecmp(module_name, "servo") == 0)
                category = HELP_CATEGORY_SERVO;
            else if (strcasecmp(module_name, "audio") == 0)
                category = HELP_CATEGORY_AUDIO;
            else if (strcasecmp(module_name, "sensor") == 0)
                category = HELP_CATEGORY_SENSOR;
            else if (strcasecmp(module_name, "comm") == 0)
                category = HELP_CATEGORY_COMM;
            else if (strcasecmp(module_name, "arm") == 0)
                category = HELP_CATEGORY_ARM;
            else if (strcasecmp(module_name, "debug") == 0)
                category = HELP_CATEGORY_DEBUG;
            else if (strcasecmp(module_name, "radar") == 0)
                category = HELP_CATEGORY_RADAR;
            else if (strcasecmp(module_name, "lvgl") == 0)
                category = HELP_CATEGORY_LVGL;
            else if (strcasecmp(module_name, "param") == 0)
                category = HELP_CATEGORY_PARAM;
            else if (strcasecmp(module_name, "motion") == 0)
                category = HELP_CATEGORY_MOTION;

            int command_count = 0;
            for (int j = 0; j < s_command_count; j++) {
                if (s_commands[j].category == category) {
                    command_count++;
                }
            }

            if (command_count == 0) {
                printf("\033[1;33m  [No commands registered]\033[0m\n");
                printf("\n  This module has no console commands yet.\n\n");
            } else {
                if (s_modules[i].module_help_command != NULL) {
                    printf("Module-specific help: %s\n\n", s_modules[i].module_help_command);
                }

                printf("\033[1;37m  Commands in this module:\033[0m\n");

                for (int j = 0; j < s_command_count; j++) {
                    if (s_commands[j].category == category) {
                        printf("  \033[0m%-30s %s\n", s_commands[j].command, s_commands[j].help);
                    }
                }
            }

            printf("\033[1;37m========================================\033[0m\n\n");
            return;
        }
    }

    printf("Module '%s' not found. Available modules:\n", module_name);
    for (int i = 0; i < s_module_count; i++) {
        printf("  - %s\n", s_modules[i].name);
    }
}

static void print_command_help(const char *command_name)
{
    for (int i = 0; i < s_command_count; i++) {
        if (strcasecmp(s_commands[i].command, command_name) == 0) {
            printf("\n\033[1;37m========================================\033[0m\n");
            printf("\033[1;37m  Command: %s\033[0m\n", s_commands[i].command);
            printf("\033[1;37m========================================\033[0m\n");
            printf("\nCategory: %s\n", s_category_names[s_commands[i].category]);
            printf("Help: %s\n", s_commands[i].help);
            printf("\033[1;37m========================================\033[0m\n\n");
            return;
        }
    }

    printf("Command '%s' not found. Use 'help' to see all available commands.\n", command_name);
}

static void search_commands(const char *keyword)
{
    printf("\n\033[1;37m========================================\033[0m\n");
    printf("\033[1;37m  Search Results for: %s\033[0m\n", keyword);
    printf("\033[1;37m========================================\033[0m\n\n");

    int found = 0;

    for (int i = 0; i < s_command_count; i++) {
        if (strcasestr(s_commands[i].command, keyword) != NULL ||
            strcasestr(s_commands[i].help, keyword) != NULL) {
            printf("  %s%-30s\033[0m %s\n",
                   s_category_colors[s_commands[i].category],
                   s_commands[i].command, s_commands[i].help);
            found++;
        }
    }

    if (found == 0) {
        printf("  No commands found matching '%s'\n", keyword);
    } else {
        printf("\n  Found %d command(s)\n", found);
    }

    printf("\033[1;37m========================================\033[0m\n\n");
}

static struct {
    struct arg_str *command;
    struct arg_str *search;
    struct arg_end *end;
} help_args;

static int cmd_help(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&help_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, help_args.end, argv[0]);
        return 1;
    }

    if (help_args.search->count > 0) {
        search_commands(help_args.search->sval[0]);
    } else if (help_args.command->count > 0) {
        const char *arg = help_args.command->sval[0];

        bool is_module = false;
        for (int i = 0; i < s_module_count; i++) {
            if (strcasecmp(s_modules[i].name, arg) == 0) {
                is_module = true;
                break;
            }
        }

        if (is_module) {
            print_module_help(arg);
        } else {
            print_command_help(arg);
        }
    } else {
        print_all_commands();
    }

    return 0;
}

static void register_help_command(void)
{
    help_args.command = arg_str0(NULL, NULL, "<command|module>", "Command or module name");
    help_args.search  = arg_str0("s", "search", "<keyword>", "Search commands by keyword");
    help_args.end     = arg_end(2);

    const esp_console_cmd_t cmd = {
        .command  = "help",
        .help     = "Show help for commands and modules",
        .hint     = "[<command|module>] or [--search <keyword>]",
        .func     = &cmd_help,
        .argtable = &help_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

static int cmd_modules(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("\n\033[1;37m========================================\033[0m\n");
    printf("\033[1;37m        Available Modules\033[0m\n");
    printf("\033[1;37m========================================\033[0m\n\n");

    for (int i = 0; i < s_module_count; i++) {
        printf("  \033[1;36m%-15s\033[0m %s\n", s_modules[i].name, s_modules[i].description);
        if (s_modules[i].module_help_command != NULL) {
            printf("                  Help: %s\n", s_modules[i].module_help_command);
        }
        printf("\n");
    }

    printf("\033[1;37m========================================\033[0m\n\n");
    return 0;
}

static void register_modules_command(void)
{
    const esp_console_cmd_t cmd = {
        .command = "modules",
        .help    = "List all available modules",
        .func    = &cmd_modules,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

void register_unified_help_commands(void)
{
    unified_help_init();

    register_help_command();
    register_modules_command();

    unified_help_register_module("system", "System commands", NULL);
    unified_help_register_module("power", "Power management commands", NULL);
    unified_help_register_module("led", "LED control commands", NULL);
    unified_help_register_module("lcd", "LCD display commands", NULL);
    unified_help_register_module("touch", "Touch sensor commands", NULL);
    unified_help_register_module("servo", "Servo control commands", NULL);
    unified_help_register_module("audio", "Audio output commands", NULL);
    unified_help_register_module("sensor", "Sensor data commands", NULL);
    unified_help_register_module("comm", "Dual-board communication commands", NULL);
    unified_help_register_module("arm", "Mechanical arm commands", NULL);
    unified_help_register_module("debug", "Debug and test commands", NULL);
    unified_help_register_module("radar", "Radar sensor commands", NULL);
    unified_help_register_module("lvgl", "LVGL display commands", NULL);
    unified_help_register_module("param", "Runtime parameter commands", NULL);
    unified_help_register_module("motion", "Motion control commands (actions, poses, scenes)", "motion_action");

    unified_help_register_command("help", "Show this help message", HELP_CATEGORY_SYSTEM);
    unified_help_register_command("modules", "List all available modules", HELP_CATEGORY_SYSTEM);

    ESP_LOGI(TAG, "Unified help commands registered");
}
