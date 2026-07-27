#include <ctype.h>
#include <string.h>

#include "esp_console.h"
#include "esp_log.h"
#include "esp_netif.h"        // For network status check in provision command
#include "esp_xiaozhi_chat.h" // For chat config
#include "esp_xiaozhi_info.h" // For provisioning (get_info, activation code)

#include "argtable3/argtable3.h"
#include "smart_voice_control_module.h"
#include "smart_voice_control_module_config.h"
#include "smart_voice_control_module_types.h"

#if (SMART_VOICE_CONTROL_MODULE_ENABLE == 1)

static const char* TAG = "smart_voice_console";

// Test callback for event notification
static void test_event_callback(const smart_voice_event_t* event, void* user_data)
{
    (void)user_data;

    const char* event_type_str;
    switch (event->type)
    {
    case SMART_VOICE_EVENT_WAKE_WORD:
        event_type_str = "WAKE_WORD";
        printf("  [EVENT] Wake word detected: %s\n",
               event->data.wake_word.wake_word ? event->data.wake_word.wake_word : "(null)");
        break;
    case SMART_VOICE_EVENT_COMMAND_TEXT:
        event_type_str = "COMMAND_TEXT";
        printf("  [EVENT] Command text: %s\n",
               event->data.command_text.text ? event->data.command_text.text : "(null)");
        break;
    case SMART_VOICE_EVENT_AUDIO_DATA:
        event_type_str = "AUDIO_DATA";
        printf("  [EVENT] Audio data: %zu bytes\n", event->data.audio.length);
        break;
    case SMART_VOICE_EVENT_LOCAL_CMD:
        event_type_str = "LOCAL_CMD";
        if (event->data.local_cmd.command)
        {
            printf("  [EVENT] Local command: [%d] %s - %s\n", event->data.local_cmd.command->id,
                   event->data.local_cmd.command->keyword,
                   event->data.local_cmd.command->description);
        }
        break;
    case SMART_VOICE_EVENT_CLOUD_TEXT:
        event_type_str = "CLOUD_TEXT";
        printf("  [EVENT] Cloud text: %s\n",
               event->data.cloud_text.text ? event->data.cloud_text.text : "(null)");
        break;
    case SMART_VOICE_EVENT_CLOUD_EMOJI:
        event_type_str = "CLOUD_EMOJI";
        printf("  [EVENT] Cloud emoji: %s\n",
               event->data.emoji.emoji_str ? event->data.emoji.emoji_str : "(null)");
        break;
    case SMART_VOICE_EVENT_TTS_STATE:
        event_type_str = "TTS_STATE";
        printf("  [EVENT] TTS state: %d, text: %s\n", event->data.tts.state,
               event->data.tts.text ? event->data.tts.text : "(null)");
        break;
    case SMART_VOICE_EVENT_CMD_NOT_RECOGNIZED:
        event_type_str = "CMD_NOT_RECOGNIZED";
        printf("  [EVENT] Command not recognized: %s\n",
               event->data.unrecognized.text ? event->data.unrecognized.text : "(null)");
        break;
    case SMART_VOICE_EVENT_ERROR:
        event_type_str = "ERROR";
        printf("  [EVENT] Error: code=%d, msg=%s\n", event->data.error.code,
               event->data.error.message ? event->data.error.message : "(null)");
        break;
    default:
        event_type_str = "UNKNOWN";
        printf("  [EVENT] Unknown type: %d\n", event->type);
        break;
    }

    ESP_LOGI(TAG, "Event received: %s (timestamp: %lu)", event_type_str,
             (unsigned long)event->timestamp);
}

// Argument structures
static struct arg_end* status_end;
static struct arg_lit* init_verbose;
static struct arg_end* init_end;
static struct arg_lit* start_verbose;
static struct arg_end* start_end;
static struct arg_int* mode_val;
static struct arg_end* mode_end;
static struct arg_end* cmd_list_end;
static struct arg_str* find_keyword;
static struct arg_end* find_end;
static struct arg_end* reload_end;
static struct arg_end* test_event_end;
static struct arg_end* stop_end;

/**
 * @brief Command: smart_voice_status - Show module status
 */
static int cmd_smart_voice_status(int argc, char** argv)
{
    int nerrors = arg_parse(argc, argv, (void**)&status_end);
    if (nerrors != 0)
    {
        arg_print_errors(stderr, status_end, argv[0]);
        return 1;
    }

    printf("\n========================================\n");
    printf("Smart Voice Control Module Status\n");
    printf("========================================\n\n");

    // State
    smart_voice_state_t state = smart_voice_control_module_get_state();
    const char*         state_str;
    switch (state)
    {
    case SMART_VOICE_STATE_UNINIT:
        state_str = "UNINIT";
        break;
    case SMART_VOICE_STATE_INIT:
        state_str = "INIT";
        break;
    case SMART_VOICE_STATE_IDLE:
        state_str = "IDLE";
        break;
    case SMART_VOICE_STATE_WAKE_DETECTED:
        state_str = "WAKE_DETECTED";
        break;
    case SMART_VOICE_STATE_PROCESSING:
        state_str = "PROCESSING";
        break;
    case SMART_VOICE_STATE_EXECUTING:
        state_str = "EXECUTING";
        break;
    case SMART_VOICE_STATE_SPEAKING:
        state_str = "SPEAKING";
        break;
    case SMART_VOICE_STATE_ERROR:
        state_str = "ERROR";
        break;
    default:
        state_str = "UNKNOWN";
        break;
    }
    printf("State: %s\n", state_str);

    // Mode
    smart_voice_mode_t mode = smart_voice_control_module_get_mode();
    const char*        mode_str;
    switch (mode)
    {
    case SMART_VOICE_MODE_LOCAL_ONLY:
        mode_str = "LOCAL_ONLY";
        break;
    case SMART_VOICE_MODE_CLOUD_ONLY:
        mode_str = "CLOUD_ONLY";
        break;
    case SMART_VOICE_MODE_HYBRID:
        mode_str = "HYBRID";
        break;
    default:
        mode_str = "UNKNOWN";
        break;
    }
    printf("Mode: %s\n", mode_str);

    // Network status
    smart_voice_network_status_t net_status = smart_voice_control_module_get_network_status();
    const char*                  net_str;
    switch (net_status)
    {
    case SMART_VOICE_NETWORK_DISCONNECTED:
        net_str = "DISCONNECTED";
        break;
    case SMART_VOICE_NETWORK_CONNECTED:
        net_str = "CONNECTED";
        break;
    case SMART_VOICE_NETWORK_RECONNECTING:
        net_str = "RECONNECTING";
        break;
    default:
        net_str = "UNKNOWN";
        break;
    }
    printf("Network: %s\n", net_str);

    // Commands count
    int cmd_count = smart_voice_control_module_get_command_count();
    printf("Commands loaded: %d\n", cmd_count);

    // Wake word detected
    bool wake_detected = smart_voice_control_module_is_wake_word_detected();
    printf("Wake word detected: %s\n", wake_detected ? "YES" : "NO");

    printf("\n========================================\n\n");

    // Also print detailed status via module function
    smart_voice_control_module_print_status();

    return 0;
}

/**
 * @brief Command: smart_voice_init - Initialize the module
 */
static int cmd_smart_voice_init(int argc, char** argv)
{
    struct
    {
        struct arg_lit* verbose;
        struct arg_end* end;
    } args = {.verbose = init_verbose, .end = init_end};

    int nerrors = arg_parse(argc, argv, (void**)&args);
    if (nerrors != 0)
    {
        arg_print_errors(stderr, args.end, argv[0]);
        return 1;
    }

    printf("Initializing Smart Voice Control Module...\n");

    // Register test callback first
    esp_err_t ret = smart_voice_register_callback(test_event_callback, NULL);
    if (ret != ESP_OK)
    {
        printf("Warning: Failed to register callback: %s\n", esp_err_to_name(ret));
    }

    // Initialize with default config
    smart_voice_config_params_t config = {
        .mode                 = SMART_VOICE_MODE_HYBRID,
        .wake_word            = NULL, // Use default from Kconfig
        .task_stack_size      = SMART_VOICE_TASK_STACK_SIZE,
        .task_priority        = SMART_VOICE_TASK_PRIORITY,
        .enable_aec           = true,
        .enable_vad           = true,
        .enable_wake_word     = true,
        .wake_word_model      = NULL, // Use default from Kconfig
        .auto_fallback        = true,
        .cloud_retry_count    = SMART_VOICE_CLOUD_RETRY_COUNT,
        .cloud_retry_delay_ms = SMART_VOICE_CLOUD_RETRY_DELAY_MS,
        .config_path          = SMART_VOICE_CONFIG_PATH,
        .enable_hot_reload    = SMART_VOICE_ENABLE_HOT_RELOAD,
    };

    ret = smart_voice_control_module_init(&config);
    if (ret != ESP_OK)
    {
        printf("Failed to initialize: %s\n", esp_err_to_name(ret));
        return 1;
    }

    printf("Smart Voice Control Module initialized successfully!\n");

    // Auto-start after initialization (create audio input task, etc.)
    ret = smart_voice_control_module_start();
    if (ret != ESP_OK)
    {
        printf("Warning: Failed to start: %s\n", esp_err_to_name(ret));
        printf("You can manually run: smart_voice_start\n");
    }
    else
    {
        printf("Smart Voice Control Module started successfully!\n");
        printf("Say '%s' to wake up...\n", SMART_VOICE_WAKE_WORD);
    }

    if (args.verbose->count > 0)
    {
        smart_voice_control_module_print_status();
    }

    return 0;
}

/**
 * @brief Command: smart_voice_start - Start the module
 */
static int cmd_smart_voice_start(int argc, char** argv)
{
    struct
    {
        struct arg_lit* verbose;
        struct arg_end* end;
    } args = {.verbose = start_verbose, .end = start_end};

    int nerrors = arg_parse(argc, argv, (void**)&args);
    if (nerrors != 0)
    {
        arg_print_errors(stderr, args.end, argv[0]);
        return 1;
    }

    printf("Starting Smart Voice Control Module...\n");

    esp_err_t ret = smart_voice_control_module_start();
    if (ret != ESP_OK)
    {
        printf("Failed to start: %s\n", esp_err_to_name(ret));
        return 1;
    }

    printf("Smart Voice Control Module started successfully!\n");
    printf("Say '%s' to wake up...\n", SMART_VOICE_WAKE_WORD);

    if (args.verbose->count > 0)
    {
        smart_voice_control_module_print_status();
    }

    return 0;
}

/**
 * @brief Command: smart_voice_stop - Stop the module
 */
static int cmd_smart_voice_stop(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    printf("Stopping Smart Voice Control Module...\n");

    esp_err_t ret = smart_voice_control_module_stop();
    if (ret != ESP_OK)
    {
        printf("Failed to stop: %s\n", esp_err_to_name(ret));
        return 1;
    }

    printf("Smart Voice Control Module stopped.\n");
    return 0;
}

/**
 * @brief Command: smart_voice_mode - Set/get operation mode
 */
static int cmd_smart_voice_mode(int argc, char** argv)
{
    struct
    {
        struct arg_int* mode;
        struct arg_end* end;
    } args = {.mode = mode_val, .end = mode_end};

    int nerrors = arg_parse(argc, argv, (void**)&args);
    if (nerrors != 0)
    {
        arg_print_errors(stderr, args.end, argv[0]);
        return 1;
    }

    if (args.mode->count > 0)
    {
        // Set mode
        int mode_value = args.mode->ival[0];
        if (mode_value < 0 || mode_value > SMART_VOICE_MODE_HYBRID)
        {
            printf("Invalid mode value. Use: 0=LOCAL_ONLY, 1=CLOUD_ONLY, 2=HYBRID\n");
            return 1;
        }

        smart_voice_mode_t new_mode = (smart_voice_mode_t)mode_value;
        esp_err_t          ret      = smart_voice_set_mode(new_mode);
        if (ret != ESP_OK)
        {
            printf("Failed to set mode: %s\n", esp_err_to_name(ret));
            return 1;
        }

        const char* mode_str;
        switch (new_mode)
        {
        case SMART_VOICE_MODE_LOCAL_ONLY:
            mode_str = "LOCAL_ONLY";
            break;
        case SMART_VOICE_MODE_CLOUD_ONLY:
            mode_str = "CLOUD_ONLY";
            break;
        case SMART_VOICE_MODE_HYBRID:
            mode_str = "HYBRID";
            break;
        default:
            mode_str = "UNKNOWN";
            break;
        }
        printf("Mode set to: %s (%d)\n", mode_str, mode_value);
    }
    else
    {
        // Get current mode
        smart_voice_mode_t current_mode = smart_voice_get_mode();
        const char*        mode_str;
        switch (current_mode)
        {
        case SMART_VOICE_MODE_LOCAL_ONLY:
            mode_str = "LOCAL_ONLY (0)";
            break;
        case SMART_VOICE_MODE_CLOUD_ONLY:
            mode_str = "CLOUD_ONLY (1)";
            break;
        case SMART_VOICE_MODE_HYBRID:
            mode_str = "HYBRID (2)";
            break;
        default:
            mode_str = "UNKNOWN (-1)";
            break;
        }
        printf("Current mode: %s\n", mode_str);
    }

    return 0;
}

/**
 * @brief Command: smart_voice_cmds - List all loaded commands
 */
static int cmd_smart_voice_cmds(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    int count = smart_voice_control_module_get_command_count();

    printf("\n========================================\n");
    printf("Loaded Voice Commands (%d)\n", count);
    printf("========================================\n\n");

    for (int i = 0; i < count; i++)
    {
        const smart_voice_command_t* cmd = smart_voice_control_module_get_command(i);
        if (cmd == NULL)
        {
            continue;
        }

        const char* type_str;
        switch (cmd->type)
        {
        case SMART_VOICE_CMD_TYPE_PAGE_NAVIGATE:
            type_str = "PAGE_NAV";
            break;
        case SMART_VOICE_CMD_TYPE_EXPRESSION:
            type_str = "EXPR";
            break;
        case SMART_VOICE_CMD_TYPE_LED_CONTROL:
            type_str = "LED";
            break;
        case SMART_VOICE_CMD_TYPE_SERVO_CONTROL:
            type_str = "SERVO";
            break;
        case SMART_VOICE_CMD_TYPE_LIGHT_CONTROL:
            type_str = "LIGHT";
            break;
        case SMART_VOICE_CMD_TYPE_SYSTEM_CMD:
            type_str = "SYSTEM";
            break;
        case SMART_VOICE_CMD_TYPE_CUSTOM:
            type_str = "CUSTOM";
            break;
        default:
            type_str = "UNKNOWN";
            break;
        }

        printf("[%2d] %-12s | %-16s | %s\n", cmd->id, type_str, cmd->keyword, cmd->description);
        printf("     Action: %s | Priority: %d | Enabled: %s\n\n", cmd->action, cmd->priority,
               cmd->enabled ? "YES" : "NO");
    }

    printf("========================================\n\n");
    return 0;
}

/**
 * @brief Helper function for case-insensitive substring search
 */
static bool str_contains(const char* haystack, const char* needle)
{
    if (haystack == NULL || needle == NULL)
        return false;

    // Convert both strings to lowercase for comparison
    char* h_lower = strdup(haystack);
    char* n_lower = strdup(needle);
    if (h_lower == NULL || n_lower == NULL)
    {
        free(h_lower);
        free(n_lower);
        return false;
    }

    for (char* p = h_lower; *p; p++)
        *p = tolower(*p);
    for (char* p = n_lower; *p; p++)
        *p = tolower(*p);

    bool found = (strstr(h_lower, n_lower) != NULL);

    free(h_lower);
    free(n_lower);
    return found;
}

/**
 * @brief Command: smart_voice_find - Find command by keyword
 */
static int cmd_smart_voice_find(int argc, char** argv)
{
    struct
    {
        struct arg_str* keyword;
        struct arg_end* end;
    } args = {.keyword = find_keyword, .end = find_end};

    int nerrors = arg_parse(argc, argv, (void**)&args);
    if (nerrors != 0)
    {
        arg_print_errors(stderr, args.end, argv[0]);
        return 1;
    }

    if (args.keyword->count == 0)
    {
        printf("Usage: smart_voice_find <keyword>\n");
        printf("Examples:\n");
        printf("  smart_voice_find main\n");
        printf("  smart_voice_find smile\n");
        printf("  smart_voice_find reboot\n");
        return 1;
    }

    const char*                  keyword = args.keyword->sval[0];
    const smart_voice_command_t* cmd     = smart_voice_control_module_find_command(keyword);

    // If exact match failed, try fuzzy search across all fields
    if (cmd == NULL)
    {
        int count = smart_voice_control_module_get_command_count();

        for (int i = 0; i < count; i++)
        {
            const smart_voice_command_t* candidate = smart_voice_control_module_get_command(i);
            if (candidate == NULL)
                continue;

            // Search in keyword, description, and action fields
            if (str_contains(candidate->keyword, keyword) ||
                str_contains(candidate->description, keyword) ||
                str_contains(candidate->action, keyword))
            {
                cmd = candidate;
                break;
            }
        }
    }

    if (cmd == NULL)
    {
        printf("Command not found: '%s'\n", keyword);
        printf("\nAvailable commands (use 'smart_voice_cmds' for full list):\n");
        printf("  main     - 打开主界面 (Navigate to main page)\n");
        printf("  settings - 打开设置 (Navigate to settings)\n");
        printf("  prev     - 上一页 (Previous page)\n");
        printf("  next     - 下一页 (Next page)\n");
        printf("  smile    - 显示笑脸 (Show smile)\n");
        printf("  sad      - 显示哭脸 (Show sad)\n");
        printf("  angry    - 显示生气 (Show angry)\n");
        printf("  surprise - 显示惊讶 (Show surprised)\n");
        printf("  reboot   - 重启设备 (Reboot device)\n");
        return 0;
    }

    printf("\nCommand Found:\n");
    printf("  ID: %d\n", cmd->id);
    printf("  Keyword: %s\n", cmd->keyword);
    printf("  Description: %s\n", cmd->description);
    printf("  Type: %d\n", cmd->type);
    printf("  Action: %s\n", cmd->action);
    printf("  Priority: %d\n", cmd->priority);
    printf("  Enabled: %s\n", cmd->enabled ? "YES" : "NO");

    return 0;
}

/**
 * @brief Command: smart_voice_reload - Reload commands from config file
 */
static int cmd_smart_voice_reload(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    printf("Reloading voice commands from config file...\n");

    esp_err_t ret = smart_voice_reload_commands();
    if (ret != ESP_OK)
    {
        printf("Failed to reload: %s\n", esp_err_to_name(ret));

        if (ret == ESP_ERR_NOT_SUPPORTED)
        {
            printf("Hint: Enable CONFIG_PROJECT_SMART_VOICE_ENABLE_HOT_RELOAD in menuconfig\n");
        }
        return 1;
    }

    printf("Commands reloaded successfully!\n");
    printf("Total commands: %d\n", smart_voice_control_module_get_command_count());

    return 0;
}

// Default voice commands JSON configuration
static const char* s_default_voice_commands_json =
    "{\n"
    "  \"version\": \"1.0.0\",\n"
    "  \"description\": \"Smart Voice Control Module - Fixed Voice Commands Configuration\",\n"
    "  \"last_modified\": \"2026-05-25T00:00:00Z\",\n"
    "  \"commands\": [\n"
    "    {\n"
    "      \"id\": 1,\n"
    "      \"keyword\": \"打开主界面\",\n"
    "      \"description\": \"Navigate to main page\",\n"
    "      \"type\": \"PAGE_NAVIGATE\",\n"
    "      \"action\": \"{\\\"page\\\": \\\"main\\\"}\",\n"
    "      \"priority\": 1,\n"
    "      \"enabled\": true\n"
    "    },\n"
    "    {\n"
    "      \"id\": 2,\n"
    "      \"keyword\": \"打开设置\",\n"
    "      \"description\": \"Navigate to settings page\",\n"
    "      \"type\": \"PAGE_NAVIGATE\",\n"
    "      \"action\": \"{\\\"page\\\": \\\"settings\\\"}\",\n"
    "      \"priority\": 1,\n"
    "      \"enabled\": true\n"
    "    },\n"
    "    {\n"
    "      \"id\": 3,\n"
    "      \"keyword\": \"上一页\",\n"
    "      \"description\": \"Navigate to previous page\",\n"
    "      \"type\": \"PAGE_NAVIGATE\",\n"
    "      \"action\": \"{\\\"page\\\": \\\"prev\\\"}\",\n"
    "      \"priority\": 2,\n"
    "      \"enabled\": true\n"
    "    },\n"
    "    {\n"
    "      \"id\": 4,\n"
    "      \"keyword\": \"下一页\",\n"
    "      \"description\": \"Navigate to next page\",\n"
    "      \"type\": \"PAGE_NAVIGATE\",\n"
    "      \"action\": \"{\\\"page\\\": \\\"next\\\"}\",\n"
    "      \"priority\": 2,\n"
    "      \"enabled\": true\n"
    "    },\n"
    "    {\n"
    "      \"id\": 5,\n"
    "      \"keyword\": \"显示笑脸\",\n"
    "      \"description\": \"Show smile expression on LCD\",\n"
    "      \"type\": \"EXPRESSION\",\n"
    "      \"action\": \"{\\\"expression\\\": \\\"smile\\\"}\",\n"
    "      \"priority\": 3,\n"
    "      \"enabled\": true\n"
    "    },\n"
    "    {\n"
    "      \"id\": 6,\n"
    "      \"keyword\": \"显示哭脸\",\n"
    "      \"description\": \"Show sad expression on LCD\",\n"
    "      \"type\": \"EXPRESSION\",\n"
    "      \"action\": \"{\\\"expression\\\": \\\"sad\\\"}\",\n"
    "      \"priority\": 3,\n"
    "      \"enabled\": true\n"
    "    },\n"
    "    {\n"
    "      \"id\": 7,\n"
    "      \"keyword\": \"显示生气\",\n"
    "      \"description\": \"Show angry expression on LCD\",\n"
    "      \"type\": \"EXPRESSION\",\n"
    "      \"action\": \"{\\\"expression\\\": \\\"angry\\\"}\",\n"
    "      \"priority\": 3,\n"
    "      \"enabled\": true\n"
    "    },\n"
    "    {\n"
    "      \"id\": 8,\n"
    "      \"keyword\": \"显示惊讶\",\n"
    "      \"description\": \"Show surprised expression on LCD\",\n"
    "      \"type\": \"EXPRESSION\",\n"
    "      \"action\": \"{\\\"expression\\\": \\\"surprised\\\"}\",\n"
    "      \"priority\": 3,\n"
    "      \"enabled\": true\n"
    "    },\n"
    "    {\n"
    "      \"id\": 9,\n"
    "      \"keyword\": \"重启设备\",\n"
    "      \"description\": \"Reboot the device\",\n"
    "      \"type\": \"SYSTEM_CMD\",\n"
    "      \"action\": \"{\\\"command\\\": \\\"reboot\\\"}\",\n"
    "      \"priority\": 10,\n"
    "      \"enabled\": true\n"
    "    }\n"
    "  ]\n"
    "}\n";

/**
 * @brief Command: smart_voice_import_config - Import default config file to LittleFS
 */
static int cmd_smart_voice_import_config(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    printf("Importing default voice commands configuration...\n\n");

    // Create /config directory if it doesn't exist
    extern esp_err_t audio_storage_mkdir(const char* path);
    esp_err_t        ret = audio_storage_mkdir("/config");
    if (ret != ESP_OK)
    {
        printf("Note: /config directory may already exist (error: %s)\n", esp_err_to_name(ret));
        // Continue anyway - directory might already exist
    }
    else
    {
        printf("Created /config directory\n");
    }

    // Write default JSON config
    size_t           json_len = strlen(s_default_voice_commands_json);
    extern esp_err_t audio_storage_write_file(const char* path, const uint8_t* data, size_t size);
    ret = audio_storage_write_file("/config/voice_commands.json",
                                   (const uint8_t*)s_default_voice_commands_json, json_len);

    if (ret != ESP_OK)
    {
        printf("Failed to write config file: %s\n", esp_err_to_name(ret));
        return 1;
    }

    printf("Config file written successfully (%zu bytes)\n", json_len);
    printf("\nTo reload the new configuration, run:\n");
    printf("  smart_voice_reload\n");

    return 0;
}

/**
 * @brief Command: smart_voice_test_event - Send a test event
 */
static int cmd_smart_voice_test_event(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    printf("Sending test events...\n\n");

    // Static buffers for test data
    static char    test_wake_word[32] = "小智小智";
    static int16_t test_audio[160]    = {0};

    // Test 1: Wake word event
    smart_voice_event_t wake_event      = {.type      = SMART_VOICE_EVENT_WAKE_WORD,
                                           .timestamp = xTaskGetTickCount() * portTICK_PERIOD_MS};
    wake_event.data.wake_word.wake_word = test_wake_word;

    esp_err_t ret = smart_voice_push_event(&wake_event);
    printf("Test 1 - Wake Word Event: %s\n", ret == ESP_OK ? "OK" : esp_err_to_name(ret));

    // Test 2: Local command event
    smart_voice_event_t cmd_event  = {.type      = SMART_VOICE_EVENT_LOCAL_CMD,
                                      .timestamp = xTaskGetTickCount() * portTICK_PERIOD_MS};
    cmd_event.data.command.keyword = "打开主界面";
    cmd_event.data.command.action  = "{\"page\": \"main\"}";

    ret = smart_voice_push_event(&cmd_event);
    printf("Test 2 - Local Command Event: %s\n", ret == ESP_OK ? "OK" : esp_err_to_name(ret));

    // Test 3: Audio data event
    smart_voice_event_t audio_event = {.type      = SMART_VOICE_EVENT_AUDIO_DATA,
                                       .timestamp = xTaskGetTickCount() * portTICK_PERIOD_MS};
    audio_event.data.audio.data     = (uint8_t*)test_audio;
    audio_event.data.audio.length   = sizeof(test_audio);

    ret = smart_voice_push_event(&audio_event);
    printf("Test 3 - Audio Data Event: %s\n", ret == ESP_OK ? "OK" : esp_err_to_name(ret));

    printf("\nAll test events sent!\n");
    return 0;
}

/**
 * @brief Command: smart_voice_provision - Get xiaozhi activation code for provisioning
 */
static int cmd_smart_voice_provision(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    printf("========================================\n");
    printf("   XiaoZhi Provisioning (配网)\n");
    printf("========================================\n\n");

    // Check actual network status (not module mode!)
    // Module may be in LOCAL_ONLY due to missing config, but network could be connected
    bool network_connected = false;

    // Try to get IP address to verify network connectivity
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL)
    {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK)
        {
            // Check if we have a valid IP (not 0.0.0.0)
            if (ip_info.ip.addr != 0)
            {
                network_connected = true;
                printf("✅ Network connected: " IPSTR "\n", IP2STR(&ip_info.ip));
            }
        }
    }

    if (!network_connected)
    {
        printf("⚠️  No network connection detected!\n");
        printf("   Please ensure WiFi is connected first.\n");
        printf("\nTip: Use 'wifi_connect' or wait for auto-connect.\n");
        printf("      Then run 'smart_voice_provision' again.\n\n");
        return 1;
    }

    printf("Step 1: Requesting device info from xiaozhi.me server...\n");

    esp_xiaozhi_chat_info_t info = {0};
    esp_err_t               ret  = esp_xiaozhi_chat_get_info(&info);

    if (ret != ESP_OK)
    {
        printf("❌ Failed to get device info: %s\n", esp_err_to_name(ret));
        printf("\nPossible causes:\n");
        printf("  - Network not connected\n");
        printf("  - Server unreachable (check firewall/proxy)\n");
        printf("  - DNS resolution failed\n");
        return 1;
    }

    printf("✅ Device info received successfully!\n\n");

    // Print device info
    printf("--- Device Information ---\n");
    if (info.serial_number && strlen(info.serial_number) > 0)
    {
        printf("Serial Number: %s\n", info.serial_number);
    }
    if (info.current_version && strlen(info.current_version) > 0)
    {
        printf("Current Version: %s\n", info.current_version);
    }

    // Print activation code
    if (info.has_activation_code && info.activation_code)
    {
        printf("\n🎯 ========================================\n");
        printf("   ACTIVATION CODE (激活码)\n");
        printf("   ========================================\n");
        printf("\n   ⭐ %s ⭐\n", info.activation_code);
        printf("\n========================================\n\n");

        if (info.activation_message && strlen(info.activation_message) > 0)
        {
            printf("Message: %s\n\n", info.activation_message);
        }

        printf("Step 2: Please complete the following steps:\n");
        printf("  1. Open browser and visit: https://xiaozhi.me\n");
        printf("  2. Login / Register your account\n");
        printf("  3. Go to 'My Devices' → 'Add Device'\n");
        printf("  4. Enter the activation code above\n");
        printf("  5. Wait for configuration to complete\n\n");

        if (info.activation_timeout_ms > 0)
        {
            printf("⏱️  Timeout: %ld seconds\n", (long)(info.activation_timeout_ms / 1000));
            printf("   Please complete pairing within this time!\n\n");
        }

        // Check if websocket config was received
        if (info.has_websocket_config)
        {
            printf("✅ WebSocket config received and saved to NVS!\n");
            printf("   You can now use: smart_voice_start\n");
        }
        else
        {
            printf("⏳ WebSocket config pending...\n");
            printf("   After entering code on website, run this command again:\n");
            printf("   smart_voice_provision\n");
        }

        if (info.has_mqtt_config)
        {
            printf("✅ MQTT config also received!\n");
        }
    }
    else
    {
        printf("\n⚠️  No activation code in response.\n");
        printf("   The device might already be activated.\n");
        printf("   WebSocket config: %s\n", info.has_websocket_config ? "YES ✅" : "NO ❌");
        printf("   MQTT config: %s\n", info.has_mqtt_config ? "YES ✅" : "NO ❌");

        if (!info.has_websocket_config && !info.has_mqtt_config)
        {
            printf("\n❌ No config received. Please check your xiaozhi.me account.\n");
        }
    }

    // Print firmware update info
    if (info.has_new_version && info.firmware_url)
    {
        printf("\n--- Firmware Update Available ---\n");
        printf("New Version: %s\n", info.firmware_version ? info.firmware_version : "Unknown");
        printf("Download URL: %s\n", info.firmware_url);
    }

    esp_xiaozhi_chat_free_info(&info);

    printf("\n========================================\n");
    return 0;
}

// Argument for provision command
static struct arg_end* provision_end = NULL;

/**
 * @brief Register all smart voice control console commands
 */
esp_err_t smart_voice_register_console_commands(void)
{
    // Initialize argument tables
    status_end    = arg_end(1);
    init_verbose  = arg_litn("v", "verbose", 0, 1, "Show detailed output after init");
    init_end      = arg_end(1);
    start_verbose = arg_litn("v", "verbose", 0, 1, "Show detailed output after start");
    start_end     = arg_end(1);
    mode_val = arg_intn(NULL, NULL, "<0-2>", 0, 1, "Mode: 0=LOCAL_ONLY, 1=CLOUD_ONLY, 2=HYBRID");
    mode_end = arg_end(1);
    cmd_list_end   = arg_end(1);
    find_keyword   = arg_strn(NULL, NULL, "<keyword>", 0, 1, "Keyword to search (English)");
    find_end       = arg_end(1);
    reload_end     = arg_end(1);
    test_event_end = arg_end(1);
    provision_end  = arg_end(1);
    stop_end       = arg_end(1);

    // Register commands
    const esp_console_cmd_t status_cmd = {
        .command  = "smart_voice_status",
        .help     = "Show Smart Voice Control Module status",
        .func     = &cmd_smart_voice_status,
        .argtable = &status_end,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&status_cmd));

    const esp_console_cmd_t init_cmd = {
        .command  = "smart_voice_init",
        .help     = "Initialize Smart Voice Control Module [-v for verbose]",
        .func     = &cmd_smart_voice_init,
        .argtable = (void*[]){init_verbose, init_end},
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&init_cmd));

    const esp_console_cmd_t start_cmd = {
        .command  = "smart_voice_start",
        .help     = "Start Smart Voice Control Module [-v for verbose]",
        .func     = &cmd_smart_voice_start,
        .argtable = (void*[]){start_verbose, start_end},
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&start_cmd));

    const esp_console_cmd_t stop_cmd = {
        .command  = "smart_voice_stop",
        .help     = "Stop Smart Voice Control Module",
        .func     = &cmd_smart_voice_stop,
        .argtable = &stop_end,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&stop_cmd));

    const esp_console_cmd_t mode_cmd = {
        .command  = "smart_voice_mode",
        .help     = "Get/Set operation mode (0=LOCAL_ONLY, 1=CLOUD_ONLY, 2=HYBRID)",
        .func     = &cmd_smart_voice_mode,
        .argtable = (void*[]){mode_val, mode_end},
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&mode_cmd));

    const esp_console_cmd_t cmds_cmd = {
        .command  = "smart_voice_cmds",
        .help     = "List all loaded voice commands",
        .func     = &cmd_smart_voice_cmds,
        .argtable = &cmd_list_end,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmds_cmd));

    const esp_console_cmd_t find_cmd = {
        .command  = "smart_voice_find",
        .help     = "Find command by keyword (use English: main, smile, reboot, etc.)",
        .func     = &cmd_smart_voice_find,
        .argtable = (void*[]){find_keyword, find_end},
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&find_cmd));

    const esp_console_cmd_t reload_cmd = {
        .command  = "smart_voice_reload",
        .help     = "Reload voice commands from config file",
        .func     = &cmd_smart_voice_reload,
        .argtable = &reload_end,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&reload_cmd));

    const esp_console_cmd_t import_config_cmd = {
        .command  = "smart_voice_import_config",
        .help     = "Import default voice commands config to LittleFS",
        .func     = &cmd_smart_voice_import_config,
        .argtable = NULL,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&import_config_cmd));

    const esp_console_cmd_t test_event_cmd = {
        .command  = "smart_voice_test_event",
        .help     = "Send test events to verify event system",
        .func     = &cmd_smart_voice_test_event,
        .argtable = &test_event_end,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&test_event_cmd));

    const esp_console_cmd_t provision_cmd = {
        .command  = "smart_voice_provision",
        .help     = "Get xiaozhi activation code for cloud provisioning (配网)",
        .func     = &cmd_smart_voice_provision,
        .argtable = &provision_end,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&provision_cmd));

    ESP_LOGI(TAG, "Smart Voice Control Module console commands registered");
    return ESP_OK;
}

#else

esp_err_t smart_voice_register_console_commands(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif // SMART_VOICE_CONTROL_MODULE_ENABLE
