#include "esp_console.h"
#include "esp_log.h"

#include "argtable3/argtable3.h"
#include "voice_control_module.h"
#include "voice_control_module_config.h"

#if (VOICE_CONTROL_MODULE_ENABLE == 1)

static const char* TAG = "voice_console";

static struct
{
    struct arg_end* end;
} voice_status_args;

static struct
{
    struct arg_end* end;
} voice_start_args;

static struct
{
    struct arg_end* end;
} voice_stop_args;

static struct
{
    struct arg_end* end;
} voice_help_args;

static int cmd_voice_status(int argc, char** argv)
{
    int nerrors = arg_parse(argc, argv, (void**)&voice_status_args);
    if (nerrors != 0)
    {
        arg_print_errors(stderr, voice_status_args.end, argv[0]);
        return 1;
    }

    voice_control_module_state_t state = voice_control_module_get_state();

    printf("\n========================================\n");
    printf("Voice Control Module Status\n");
    printf("========================================\n");

    switch (state)
    {
    case VOICE_CONTROL_STATE_UNINIT:
        printf("State: UNINIT (Not initialized)\n");
        break;
    case VOICE_CONTROL_STATE_IDLE:
        printf("State: IDLE (Initialized, waiting to start)\n");
        break;
    case VOICE_CONTROL_STATE_CONNECTING:
        printf("State: CONNECTING (Connecting to server)\n");
        break;
    case VOICE_CONTROL_STATE_LISTENING:
        printf("State: LISTENING (Listening for wake word)\n");
        break;
    case VOICE_CONTROL_STATE_SPEAKING:
        printf("State: SPEAKING (Processing speech)\n");
        break;
    case VOICE_CONTROL_STATE_ERROR:
        printf("State: ERROR\n");
        break;
    default:
        printf("State: UNKNOWN\n");
        break;
    }

    printf("========================================\n\n");
    return 0;
}

static int cmd_voice_start(int argc, char** argv)
{
    int nerrors = arg_parse(argc, argv, (void**)&voice_start_args);
    if (nerrors != 0)
    {
        arg_print_errors(stderr, voice_start_args.end, argv[0]);
        return 1;
    }

    voice_control_module_state_t state = voice_control_module_get_state();

    if (state == VOICE_CONTROL_STATE_LISTENING)
    {
        printf("Voice control module is already running\n");
        return 0;
    }

    if (state == VOICE_CONTROL_STATE_UNINIT)
    {
        printf("Initializing voice control module...\n");

        voice_control_module_config_t config = VOICE_CONTROL_MODULE_DEFAULT_CONFIG();
        esp_err_t                     ret    = voice_control_module_init(&config);
        if (ret != ESP_OK)
        {
            printf("Failed to initialize voice control module: %s\n", esp_err_to_name(ret));
            return 1;
        }
        printf("Voice control module initialized successfully\n");
    }

    printf("Starting voice control module...\n");
    esp_err_t ret = voice_control_module_start();
    if (ret != ESP_OK)
    {
        printf("Failed to start voice control module: %s\n", esp_err_to_name(ret));
        return 1;
    }

    printf("Voice control module started successfully\n");
    printf("Say '小智小智' to wake up, then say a command\n");
    return 0;
}

static int cmd_voice_stop(int argc, char** argv)
{
    int nerrors = arg_parse(argc, argv, (void**)&voice_stop_args);
    if (nerrors != 0)
    {
        arg_print_errors(stderr, voice_stop_args.end, argv[0]);
        return 1;
    }

    voice_control_module_state_t state = voice_control_module_get_state();

    if (state == VOICE_CONTROL_STATE_UNINIT)
    {
        printf("Voice control module is not initialized\n");
        return 0;
    }

    if (state == VOICE_CONTROL_STATE_LISTENING || state == VOICE_CONTROL_STATE_SPEAKING ||
        state == VOICE_CONTROL_STATE_CONNECTING)
    {
        printf("Stopping voice control module...\n");
        esp_err_t ret = voice_control_module_stop();
        if (ret != ESP_OK)
        {
            printf("Failed to stop voice control module: %s\n", esp_err_to_name(ret));
            return 1;
        }
        printf("Voice control module stopped\n");
    }

    printf("Deinitializing voice control module...\n");
    esp_err_t ret = voice_control_module_deinit();
    if (ret != ESP_OK)
    {
        printf("Failed to deinitialize voice control module: %s\n", esp_err_to_name(ret));
        return 1;
    }

    printf("Voice control module deinitialized successfully\n");
    return 0;
}

static int cmd_voice_help(int argc, char** argv)
{
    int nerrors = arg_parse(argc, argv, (void**)&voice_help_args);
    if (nerrors != 0)
    {
        arg_print_errors(stderr, voice_help_args.end, argv[0]);
        return 1;
    }

    printf("\n========================================\n");
    printf("Voice Control Module - Supported Commands\n");
    printf("========================================\n\n");

    printf("Wake Word: 小智小智 (Xiao Zhi Xiao Zhi)\n\n");

    printf("Supported Commands:\n");
    printf("  1. 切换页面 (qie huan ye mian) - Switch LCD page\n");
    printf("  2. 显示开心 (xian shi kai xin) - Show happy expression\n");
    printf("  3. 显示正常 (xian shi zheng chang) - Show normal expression\n");
    printf("  4. 显示睡眠 (xian shi shui mian) - Show sleepy expression\n");
    printf("  5. 开启眨眼 (kai qi zha yan) - Enable auto blink\n");
    printf("  6. 关闭眨眼 (guan bi zha yan) - Disable auto blink\n\n");

    printf("Usage:\n");
    printf("  1. Say wake word: '小智小智'\n");
    printf("  2. After wake word detected, say a command\n");
    printf("  3. The command will be executed automatically\n\n");

    printf("========================================\n\n");
    return 0;
}

esp_err_t voice_control_register_console_commands(void)
{
    voice_status_args.end              = arg_end(1);
    const esp_console_cmd_t status_cmd = {
        .command  = "voice_status",
        .help     = "Show voice control module status",
        .func     = &cmd_voice_status,
        .argtable = &voice_status_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&status_cmd));

    voice_start_args.end              = arg_end(1);
    const esp_console_cmd_t start_cmd = {
        .command  = "voice_start",
        .help     = "Start voice control module",
        .func     = &cmd_voice_start,
        .argtable = &voice_start_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&start_cmd));

    voice_stop_args.end              = arg_end(1);
    const esp_console_cmd_t stop_cmd = {
        .command  = "voice_stop",
        .help     = "Stop voice control module",
        .func     = &cmd_voice_stop,
        .argtable = &voice_stop_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&stop_cmd));

    voice_help_args.end              = arg_end(1);
    const esp_console_cmd_t help_cmd = {
        .command  = "voice_help",
        .help     = "Show supported voice commands",
        .func     = &cmd_voice_help,
        .argtable = &voice_help_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&help_cmd));

    ESP_LOGI(TAG, "Voice control console commands registered");
    return ESP_OK;
}

#else

esp_err_t voice_control_register_console_commands(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
