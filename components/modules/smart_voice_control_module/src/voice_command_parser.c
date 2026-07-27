#include <string.h>

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "audio_storage.h"
#include "cJSON.h"
#include "smart_voice_control_module.h"
#include "smart_voice_control_module_config.h"
#include "smart_voice_control_module_types.h"

#if (SMART_VOICE_CONTROL_MODULE_ENABLE == 1)

static const char* TAG = "voice_cmd_parser";

// Forward declarations
static esp_err_t voice_command_load_from_file(const char* path);
static void      load_default_commands(void);

// Global configuration instance
static smart_voice_config_t s_config       = {0};
static SemaphoreHandle_t    s_config_mutex = NULL;

// Default commands (used when config file not found)
static const smart_voice_command_t s_default_commands[] = {
    {
        .id          = 1,
        .keyword     = "打开主界面",
        .description = "Navigate to main page",
        .type        = SMART_VOICE_CMD_TYPE_PAGE_NAVIGATE,
        .action      = "{\"page\": \"main\"}",
        .priority    = 1,
        .enabled     = true,
    },
    {
        .id          = 2,
        .keyword     = "打开设置",
        .description = "Navigate to settings page",
        .type        = SMART_VOICE_CMD_TYPE_PAGE_NAVIGATE,
        .action      = "{\"page\": \"settings\"}",
        .priority    = 1,
        .enabled     = true,
    },
    {
        .id          = 3,
        .keyword     = "上一页",
        .description = "Navigate to previous page",
        .type        = SMART_VOICE_CMD_TYPE_PAGE_NAVIGATE,
        .action      = "{\"page\": \"prev\"}",
        .priority    = 2,
        .enabled     = true,
    },
    {
        .id          = 4,
        .keyword     = "下一页",
        .description = "Navigate to next page",
        .type        = SMART_VOICE_CMD_TYPE_PAGE_NAVIGATE,
        .action      = "{\"page\": \"next\"}",
        .priority    = 2,
        .enabled     = true,
    },
    {
        .id          = 5,
        .keyword     = "显示笑脸",
        .description = "Show smile expression on LCD",
        .type        = SMART_VOICE_CMD_TYPE_EXPRESSION,
        .action      = "{\"expression\": \"smile\"}",
        .priority    = 3,
        .enabled     = true,
    },
    {
        .id          = 6,
        .keyword     = "显示哭脸",
        .description = "Show sad expression on LCD",
        .type        = SMART_VOICE_CMD_TYPE_EXPRESSION,
        .action      = "{\"expression\": \"sad\"}",
        .priority    = 3,
        .enabled     = true,
    },
    {
        .id          = 7,
        .keyword     = "显示生气",
        .description = "Show angry expression on LCD",
        .type        = SMART_VOICE_CMD_TYPE_EXPRESSION,
        .action      = "{\"expression\": \"angry\"}",
        .priority    = 3,
        .enabled     = true,
    },
    {
        .id          = 8,
        .keyword     = "显示惊讶",
        .description = "Show surprised expression on LCD",
        .type        = SMART_VOICE_CMD_TYPE_EXPRESSION,
        .action      = "{\"expression\": \"surprised\"}",
        .priority    = 3,
        .enabled     = true,
    },
    {
        .id          = 9,
        .keyword     = "重启设备",
        .description = "Reboot the device",
        .type        = SMART_VOICE_CMD_TYPE_SYSTEM_CMD,
        .action      = "{\"command\": \"reboot\"}",
        .priority    = 10,
        .enabled     = true,
    },
};

#define DEFAULT_COMMAND_COUNT (sizeof(s_default_commands) / sizeof(s_default_commands[0]))

/**
 * @brief Convert string to command type enum
 */
static smart_voice_cmd_type_t string_to_cmd_type(const char* type_str)
{
    if (strcmp(type_str, "PAGE_NAVIGATE") == 0)
        return SMART_VOICE_CMD_TYPE_PAGE_NAVIGATE;
    if (strcmp(type_str, "EXPRESSION") == 0)
        return SMART_VOICE_CMD_TYPE_EXPRESSION;
    if (strcmp(type_str, "LED_CONTROL") == 0)
        return SMART_VOICE_CMD_TYPE_LED_CONTROL;
    if (strcmp(type_str, "SERVO_CONTROL") == 0)
        return SMART_VOICE_CMD_TYPE_SERVO_CONTROL;
    if (strcmp(type_str, "LIGHT_CONTROL") == 0)
        return SMART_VOICE_CMD_TYPE_LIGHT_CONTROL;
    if (strcmp(type_str, "SYSTEM_CMD") == 0)
        return SMART_VOICE_CMD_TYPE_SYSTEM_CMD;
    if (strcmp(type_str, "CUSTOM") == 0)
        return SMART_VOICE_CMD_TYPE_CUSTOM;
    return SMART_VOICE_CMD_TYPE_UNKNOWN;
}

/**
 * @brief Parse single command from JSON object
 */
static esp_err_t parse_command_from_json(const cJSON* cmd_json, smart_voice_command_t* cmd)
{
    cJSON* id          = cJSON_GetObjectItem(cmd_json, "id");
    cJSON* keyword     = cJSON_GetObjectItem(cmd_json, "keyword");
    cJSON* description = cJSON_GetObjectItem(cmd_json, "description");
    cJSON* type        = cJSON_GetObjectItem(cmd_json, "type");
    cJSON* action      = cJSON_GetObjectItem(cmd_json, "action");
    cJSON* priority    = cJSON_GetObjectItem(cmd_json, "priority");
    cJSON* enabled     = cJSON_GetObjectItem(cmd_json, "enabled");

    if (!cJSON_IsNumber(id) || !cJSON_IsString(keyword) || !cJSON_IsString(type) ||
        !cJSON_IsString(action))
    {
        ESP_LOGE(TAG, "Missing required fields in command");
        return ESP_ERR_INVALID_ARG;
    }

    cmd->id = id->valueint;
    strncpy(cmd->keyword, keyword->valuestring, sizeof(cmd->keyword) - 1);

    if (cJSON_IsString(description))
    {
        strncpy(cmd->description, description->valuestring, sizeof(cmd->description) - 1);
    }

    cmd->type = string_to_cmd_type(type->valuestring);
    strncpy(cmd->action, action->valuestring, sizeof(cmd->action) - 1);

    if (cJSON_IsNumber(priority))
    {
        cmd->priority = priority->valueint;
    }
    else
    {
        cmd->priority = 5; // default priority
    }

    if (cJSON_IsBool(enabled))
    {
        cmd->enabled = cJSON_IsTrue(enabled);
    }
    else
    {
        cmd->enabled = true; // default enabled
    }

    return ESP_OK;
}

/**
 * @brief Load default commands into config structure
 */
static void load_default_commands(void)
{
    s_config.commands =
        (smart_voice_command_t*)malloc(DEFAULT_COMMAND_COUNT * sizeof(smart_voice_command_t));

    if (s_config.commands != NULL)
    {
        memcpy(s_config.commands, s_default_commands,
               DEFAULT_COMMAND_COUNT * sizeof(smart_voice_command_t));
        s_config.command_count = DEFAULT_COMMAND_COUNT;
        strncpy(s_config.version, "default", sizeof(s_config.version) - 1);
        ESP_LOGI(TAG, "Loaded %d default commands", DEFAULT_COMMAND_COUNT);
    }
    else
    {
        ESP_LOGE(TAG, "Failed to allocate memory for default commands");
        s_config.command_count = 0;
    }
}

/**
 * @brief Initialize voice command parser
 */
esp_err_t voice_command_parser_init(void)
{
    if (s_config_mutex == NULL)
    {
        s_config_mutex = xSemaphoreCreateMutex();
        if (s_config_mutex == NULL)
        {
            ESP_LOGE(TAG, "Failed to create mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    // Try to load from file first
    esp_err_t ret = voice_command_load_from_file(SMART_VOICE_CONFIG_PATH);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to load config file (%s), using defaults", esp_err_to_name(ret));
        load_default_commands();
    }

    return ESP_OK;
}

/**
 * @brief Load voice commands from JSON file
 */
esp_err_t voice_command_load_from_file(const char* path)
{
    if (path == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t* buffer      = NULL;
    size_t   buffer_size = 4096; // Max config size: 4KB

    buffer = (uint8_t*)malloc(buffer_size);
    if (buffer == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate read buffer");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = audio_storage_read_file(path, buffer, &buffer_size);
    if (ret != ESP_OK)
    {
        free(buffer);
        return ret; // File not found or read error
    }

    // Parse JSON
    cJSON* json = cJSON_Parse((char*)buffer);
    free(buffer); // Free buffer after parsing

    if (json == NULL)
    {
        const char* error_ptr = cJSON_GetErrorPtr();
        ESP_LOGE(TAG, "JSON parse error before: %s", error_ptr ? error_ptr : "unknown");
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Extract version
    cJSON* version = cJSON_GetObjectItem(json, "version");
    if (cJSON_IsString(version))
    {
        strncpy(s_config.version, version->valuestring, sizeof(s_config.version) - 1);
    }

    // Parse commands array
    cJSON* commands = cJSON_GetObjectItem(json, "commands");
    if (!cJSON_IsArray(commands))
    {
        ESP_LOGE(TAG, "'commands' field is not an array");
        cJSON_Delete(json);
        return ESP_ERR_INVALID_RESPONSE;
    }

    int count = cJSON_GetArraySize(commands);
    if (count <= 0 || count > 100)
    { // Limit max commands
        ESP_LOGE(TAG, "Invalid command count: %d", count);
        cJSON_Delete(json);
        return ESP_ERR_INVALID_SIZE;
    }

    // Allocate memory for commands
    smart_voice_command_t* new_commands =
        (smart_voice_command_t*)malloc(count * sizeof(smart_voice_command_t));

    if (new_commands == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for %d commands", count);
        cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }

    // Parse each command
    int    valid_count = 0;
    cJSON* cmd_item    = NULL;
    cJSON_ArrayForEach(cmd_item, commands)
    {
        esp_err_t parse_ret = parse_command_from_json(cmd_item, &new_commands[valid_count]);
        if (parse_ret == ESP_OK && new_commands[valid_count].enabled)
        {
            valid_count++;
        }
    }

    // Atomically update config with mutex protection
    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
    {
        if (s_config.commands != NULL)
        {
            free(s_config.commands);
        }

        s_config.commands      = new_commands;
        s_config.command_count = valid_count;

        xSemaphoreGive(s_config_mutex);

        ESP_LOGI(TAG, "Successfully loaded %d commands from '%s'", valid_count, path);
    }
    else
    {
        free(new_commands);
        ret = ESP_ERR_TIMEOUT;
        ESP_LOGE(TAG, "Failed to acquire mutex for config update");
    }

    cJSON_Delete(json);
    return ret;
}

/**
 * @brief Find command by keyword (thread-safe)
 */
const smart_voice_command_t* voice_command_find_by_keyword(const char* keyword)
{
    if (keyword == NULL || s_config.commands == NULL)
    {
        return NULL;
    }

    const smart_voice_command_t* result = NULL;

    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        for (int i = 0; i < s_config.command_count; i++)
        {
            if (strcmp(s_config.commands[i].keyword, keyword) == 0)
            {
                result = &s_config.commands[i];
                break;
            }
        }
        xSemaphoreGive(s_config_mutex);
    }

    return result;
}

/**
 * @brief Get command by index (thread-safe)
 */
const smart_voice_command_t* voice_command_get_by_index(int index)
{
    if (index < 0 || index >= s_config.command_count || s_config.commands == NULL)
    {
        return NULL;
    }

    const smart_voice_command_t* result = NULL;

    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        result = &s_config.commands[index];
        xSemaphoreGive(s_config_mutex);
    }

    return result;
}

/**
 * @brief Get total command count
 */
int voice_command_get_count(void)
{
    return s_config.command_count;
}

/**
 * @brief Reload configuration from file (for hot-reload feature)
 */
esp_err_t voice_command_reload_config(void)
{
#if (SMART_VOICE_ENABLE_HOT_RELOAD == 1)
    return voice_command_load_from_file(SMART_VOICE_CONFIG_PATH);
#else
    ESP_LOGW(TAG, "Hot reload is disabled in Kconfig");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/**
 * @brief Deinitialize voice command parser
 */
void voice_command_parser_deinit(void)
{
    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
    {
        if (s_config.commands != NULL)
        {
            free(s_config.commands);
            s_config.commands = NULL;
        }
        s_config.command_count = 0;
        memset(&s_config, 0, sizeof(s_config));
        xSemaphoreGive(s_config_mutex);
    }

    if (s_config_mutex != NULL)
    {
        vSemaphoreDelete(s_config_mutex);
        s_config_mutex = NULL;
    }

    ESP_LOGI(TAG, "Voice command parser deinitialized");
}

#endif // SMART_VOICE_CONTROL_MODULE_ENABLE
