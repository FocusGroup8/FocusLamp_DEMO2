/*
 * Console Utilities - Unified Console Command Helpers
 *
 * Provides common macros and utilities for ESP-IDF console command development:
 * - Standard header includes
 * - TAG definition helper
 * - Command registration helper
 * - Unified output wrappers
 * - Argument validation helpers
 *
 * Usage:
 *   #include "console_utils.h"
 *
 *   CONSOLE_DEFINE_TAG("my_module");
 *
 *   static int my_cmd(int argc, char **argv) {
 *       CONSOLE_CHECK_ARGC(2, "my_cmd <param>");
 *       CONSOLE_LOGI(TAG, "Processing: %s", argv[1]);
 *       return 0;
 *   }
 *
 *   void register_my_commands(void) {
 *       CONSOLE_REGISTER_CMD("my_cmd", "My command help", my_cmd);
 *   }
 */

#ifndef CONSOLE_UTILS_H
#define CONSOLE_UTILS_H

#include <stdio.h>
#include <string.h>

#include "esp_console.h"
#include "esp_log.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* ============================================================
 * Section 1: TAG Definition Helper
 * ============================================================ */

/**
 * @brief Define console module TAG for logging
 * @param name  Tag string (e.g., "radar_console")
 */
#define CONSOLE_DEFINE_TAG(name) static const char* TAG = name

/* ============================================================
 * Section 2: Command Registration Helper
 * ============================================================ */

/**
 * @brief Register a console command with standard settings
 * @param cmd_name   Command name string
 * @param help_text  Help text for the command
 * @param func_ptr   Command handler function pointer
 */
#define CONSOLE_REGISTER_CMD(cmd_name, help_text, func_ptr) \
    do                                                      \
    {                                                       \
        const esp_console_cmd_t cmd = {                     \
            .command  = cmd_name,                           \
            .help     = help_text,                          \
            .func     = func_ptr,                           \
            .argtable = NULL,                               \
        };                                                  \
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));    \
    } while (0)

/**
 * @brief Register a console command with argtable support
 * @param cmd_name   Command name string
 * @param help_text  Help text for the command
 * @param func_ptr   Command handler function pointer
 * @param arg_table  Pointer to argtable3 argument table
 */
#define CONSOLE_REGISTER_CMD_ARG(cmd_name, help_text, func_ptr, arg_table) \
    do                                                                     \
    {                                                                      \
        const esp_console_cmd_t cmd = {                                    \
            .command  = cmd_name,                                          \
            .help     = help_text,                                         \
            .func     = func_ptr,                                          \
            .argtable = arg_table,                                         \
        };                                                                 \
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));                   \
    } while (0)

/* ============================================================
 * Section 3: Unified Output Wrappers
 * ============================================================ */

/** @brief Direct printf output (for user-facing messages) */
#define CONSOLE_PRINTF(fmt, ...) printf(fmt, ##__VA_ARGS__)

/** @brief Log output with INFO level */
#define CONSOLE_LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)

/** @brief Log output with ERROR level */
#define CONSOLE_LOGE(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)

/** @brief Log output with WARNING level */
#define CONSOLE_LOGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)

/** @brief Log output with DEBUG level */
#define CONSOLE_LOGD(tag, fmt, ...) ESP_LOGD(tag, fmt, ##__VA_ARGS__)

/** @brief Log output with VERBOSE level */
#define CONSOLE_LOGV(tag, fmt, ...) ESP_LOGV(tag, fmt, ##__VA_ARGS__)

/* ============================================================
 * Section 4: Argument Validation Helpers
 * ============================================================ */

/**
 * @brief Check minimum argument count and print usage if not met
 * @param min_args  Minimum required arguments (including command name)
 * @param usage     Usage string to display on error
 * @return 1 if validation fails (caller should return this value)
 */
#define CONSOLE_CHECK_ARGC(min_args, usage)       \
    do                                            \
    {                                             \
        if (argc < (min_args))                    \
        {                                         \
            CONSOLE_PRINTF("Usage: %s\n", usage); \
            return 1;                             \
        }                                         \
    } while (0)

/**
 * @brief Check exact argument count and print usage if not met
 * @param expected_args  Expected argument count (including command name)
 * @param usage          Usage string to display on error
 * @return 1 if validation fails (caller should return this value)
 */
#define CONSOLE_CHECK_ARGC_EXACT(expected_args, usage) \
    do                                                 \
    {                                                  \
        if (argc != (expected_args))                   \
        {                                              \
            CONSOLE_PRINTF("Usage: %s\n", usage);      \
            return 1;                                  \
        }                                              \
    } while (0)

/**
 * @brief Print error message and return error code
 * @param fmt   Format string
 * @param ...   Format arguments
 * @return 1 (error code)
 */
#define CONSOLE_ERROR_RETURN(fmt, ...)                     \
    do                                                     \
    {                                                      \
        CONSOLE_PRINTF("Error: " fmt "\n", ##__VA_ARGS__); \
        return 1;                                          \
    } while (0)

/**
 * @brief Print success message
 * @param fmt   Format string
 * @param ...   Format arguments
 */
#define CONSOLE_SUCCESS(fmt, ...)                     \
    do                                                \
    {                                                 \
        CONSOLE_PRINTF("✓ " fmt "\n", ##__VA_ARGS__); \
    } while (0)

/* ============================================================
 * Section 5: Deprecated Command Marker
 * ============================================================ */

/**
 * @brief Mark a command as deprecated and suggest alternative
 * @param new_cmd  The new command to use instead
 */
#define CONSOLE_DEPRECATED(new_cmd) CONSOLE_PRINTF("[DEPRECATED] Use: %s\n", new_cmd)

#ifdef __cplusplus
}
#endif

#endif /* CONSOLE_UTILS_H */
