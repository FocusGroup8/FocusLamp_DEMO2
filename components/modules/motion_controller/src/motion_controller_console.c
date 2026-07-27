#include "motion_controller_config.h"

#if (MOTION_CONTROLLER_ENABLE == 1)

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_console.h"

#include "motion_controller.h"

static const char* TAG = "motion_console";

// ==================== Console Commands ====================

static int cmd_motion_init(int argc, char** argv)
{
    esp_err_t ret = motion_controller_init();
    printf("Motion controller init: %s\n", esp_err_to_name(ret));
    return 0;
}

static int cmd_motion_home(int argc, char** argv)
{
    if (argc > 1 && strcmp(argv[1], "async") == 0)
    {
        esp_err_t ret = motion_home_async_execute();
        printf("Async homing started: %s\n", esp_err_to_name(ret));
    }
    else
    {
        esp_err_t ret = motion_home_execute();
        printf("Homing result: %s\n", esp_err_to_name(ret));
    }
    return 0;
}

static int cmd_motion_status(int argc, char** argv)
{
    motion_controller_status_t status = motion_controller_get_status();

    printf("Motion Controller Status:\n");
    printf("  State: %d\n", status.state);
    printf("  Mode: %d\n", status.mode);
    printf("  Homing: %s\n", status.homing_completed ? "Completed" : "Not completed");
    printf("  Action Status: %d\n", status.action_status);
    if (status.current_action)
    {
        printf("  Current Action: %s\n", status.current_action);
    }

    printf("\nPose Slots:\n");
    for (int i = 0; i < POSE_SLOT_MAX; i++)
    {
        printf("  Slot %d: %s\n", i, status.pose_saved[i] ? "Saved" : "Empty");
    }

    return 0;
}

static int cmd_motion_mode(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("Current mode: %d\n", motion_controller_get_mode());
        printf("Modes: 0=Normal, 1=Focus, 2=Company, 3=Show\n");
        return 0;
    }

    int mode = atoi(argv[1]);
    if (mode < 0 || mode > 3)
    {
        printf("Invalid mode. Use 0-3.\n");
        return 1;
    }

    esp_err_t ret = motion_controller_set_mode((motion_mode_t)mode);
    printf("Mode set: %s\n", esp_err_to_name(ret));
    return 0;
}

static int cmd_motion_pose(int argc, char** argv)
{
    if (argc < 3)
    {
        printf("Usage: motion_pose <save|restore|get> <slot>\n");
        printf("Slots: 0=InteractionBefore, 1=SafeStandby, 2=Home, 3=Custom1, 4=Custom2\n");
        return 1;
    }

    int slot = atoi(argv[2]);
    if (slot < 0 || slot >= POSE_SLOT_MAX)
    {
        printf("Invalid slot. Use 0-4.\n");
        return 1;
    }

    if (strcmp(argv[1], "save") == 0)
    {
        esp_err_t ret = motion_pose_save((motion_pose_slot_t)slot);
        printf("Pose saved to slot %d: %s\n", slot, esp_err_to_name(ret));
    }
    else if (strcmp(argv[1], "restore") == 0)
    {
        esp_err_t ret = motion_pose_restore((motion_pose_slot_t)slot);
        printf("Pose restored from slot %d: %s\n", slot, esp_err_to_name(ret));
    }
    else if (strcmp(argv[1], "get") == 0)
    {
        motion_pose_t pose;
        esp_err_t ret = motion_pose_get((motion_pose_slot_t)slot, &pose);
        if (ret == ESP_OK && pose.valid)
        {
            printf("Pose slot %d:\n", slot);
            printf("  EM3: %d\n", pose.em3_pos);
            printf("  LX: [%d, %d, %d, %d]\n", pose.lx_pos[0], pose.lx_pos[1], pose.lx_pos[2], pose.lx_pos[3]);
        }
        else
        {
            printf("No valid pose in slot %d\n", slot);
        }
    }
    else
    {
        printf("Unknown command. Use save/restore/get.\n");
        return 1;
    }

    return 0;
}

static int cmd_motion_action(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("Available actions:\n");
        printf("  wave, nod, shake_head, come_here, go_back, dance\n");
        printf("  greet, breath, focus, exercise, happy, curious, sleepy\n");
        printf("  home, init\n");
        printf("\nUsage: motion_action <action_name> [async]\n");
        return 1;
    }

    const char* action = argv[1];
    bool async = (argc > 2 && strcmp(argv[2], "async") == 0);

    esp_err_t ret;
    if (async)
    {
        ret = motion_action_execute_async(action, NULL);
        printf("Async action started: %s\n", esp_err_to_name(ret));
    }
    else
    {
        ret = motion_action_execute(action, NULL);
        printf("Action result: %s\n", esp_err_to_name(ret));
    }

    return 0;
}

static int cmd_motion_stop(int argc, char** argv)
{
    if (argc > 1 && strcmp(argv[1], "emergency") == 0)
    {
        esp_err_t ret = motion_emergency_stop();
        printf("Emergency stop: %s\n", esp_err_to_name(ret));
    }
    else if (argc > 1 && strcmp(argv[1], "soft") == 0)
    {
        esp_err_t ret = motion_soft_stop();
        printf("Soft stop: %s\n", esp_err_to_name(ret));
    }
    else
    {
        esp_err_t ret = motion_action_stop();
        printf("Action stop: %s\n", esp_err_to_name(ret));
    }

    return 0;
}

static int cmd_motion_resume(int argc, char** argv)
{
    esp_err_t ret = motion_resume();
    printf("Resume: %s\n", esp_err_to_name(ret));
    return 0;
}

static int cmd_motion_voice(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("Usage: motion_voice <command>\n");
        printf("Commands: 你过来, 回去吧, 跳个舞吧, 打招呼\n");
        return 1;
    }

    esp_err_t ret = motion_voice_command_execute(argv[1]);
    printf("Voice command result: %s\n", esp_err_to_name(ret));
    return 0;
}

static int cmd_motion_scene(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("Usage: motion_scene <scene_name>\n");
        printf("Scenes: welcome, singing, exercise, focus_time\n");
        return 1;
    }

    esp_err_t ret = motion_scene_trigger(argv[1]);
    printf("Scene trigger result: %s\n", esp_err_to_name(ret));
    return 0;
}

// ==================== Register Commands ====================

void register_motion_controller_commands(void)
{
    const esp_console_cmd_t init_cmd = {
        .command = "motion_init",
        .help = "Initialize motion controller",
        .func = cmd_motion_init,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&init_cmd));

    const esp_console_cmd_t home_cmd = {
        .command = "motion_home",
        .help = "Execute homing sequence. Use 'motion_home async' for background execution",
        .func = cmd_motion_home,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&home_cmd));

    const esp_console_cmd_t status_cmd = {
        .command = "motion_status",
        .help = "Get motion controller status",
        .func = cmd_motion_status,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&status_cmd));

    const esp_console_cmd_t mode_cmd = {
        .command = "motion_mode",
        .help = "Set motion mode (0=Normal, 1=Focus, 2=Company, 3=Show)",
        .func = cmd_motion_mode,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&mode_cmd));

    const esp_console_cmd_t pose_cmd = {
        .command = "motion_pose",
        .help = "Manage poses: motion_pose <save|restore|get> <slot>",
        .func = cmd_motion_pose,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&pose_cmd));

    const esp_console_cmd_t action_cmd = {
        .command = "motion_action",
        .help = "Execute preset action: motion_action <name> [async]",
        .func = cmd_motion_action,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&action_cmd));

    const esp_console_cmd_t stop_cmd = {
        .command = "motion_stop",
        .help = "Stop motion: motion_stop [emergency|soft]",
        .func = cmd_motion_stop,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&stop_cmd));

    const esp_console_cmd_t resume_cmd = {
        .command = "motion_resume",
        .help = "Resume from emergency stop",
        .func = cmd_motion_resume,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&resume_cmd));

    const esp_console_cmd_t voice_cmd = {
        .command = "motion_voice",
        .help = "Execute voice command (reserved interface)",
        .func = cmd_motion_voice,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&voice_cmd));

    const esp_console_cmd_t scene_cmd = {
        .command = "motion_scene",
        .help = "Trigger scene action (reserved interface)",
        .func = cmd_motion_scene,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&scene_cmd));

    ESP_LOGI(TAG, "Motion controller commands registered");
}

#else

void register_motion_controller_commands(void)
{
    // Module disabled - no commands
}

#endif