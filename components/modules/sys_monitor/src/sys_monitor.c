#include "sys_monitor.h"

#include "sys_monitor_config.h"

#if (SYS_MONITOR_ENABLE == 1)

#include <stdio.h>
#include <string.h>

#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if (EVENT_BUS_ENABLE == 1)
#include "event_bus.h"
#endif

static const char* TAG = "sys_monitor";

static bool               s_initialized = false;
static esp_timer_handle_t s_alert_timer = NULL;

static void heap_info_fill(sys_monitor_heap_info_t* info)
{
    info->total_free    = esp_get_free_heap_size();
    info->internal_free = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    info->internal_largest_free_block =
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    info->internal_min_ever_free =
        heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    info->psram_free               = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    info->psram_largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    info->psram_min_ever_free      = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
}

static void alert_timer_callback(void* arg)
{
    sys_monitor_heap_info_t info;
    heap_info_fill(&info);

    if (info.internal_free < SYS_MONITOR_INTERNAL_HEAP_ERROR_THRESHOLD)
    {
        ESP_LOGE(TAG, "Internal heap CRITICAL: %u bytes free (threshold: %u)", info.internal_free,
                 SYS_MONITOR_INTERNAL_HEAP_ERROR_THRESHOLD);
#if (EVENT_BUS_ENABLE == 1)
        system_heap_alert_data_t alert_data = {
            .free_bytes       = info.internal_free,
            .threshold_bytes  = SYS_MONITOR_INTERNAL_HEAP_ERROR_THRESHOLD,
            .is_internal_heap = true,
        };
        event_bus_publish_simple(EVENT_TYPE_SYSTEM_ALERT, SYSTEM_ALERT_HEAP_CRITICAL, &alert_data,
                                 sizeof(alert_data));
#endif
    }
    else if (info.internal_free < SYS_MONITOR_INTERNAL_HEAP_WARN_THRESHOLD)
    {
        ESP_LOGW(TAG, "Internal heap LOW: %u bytes free (threshold: %u)", info.internal_free,
                 SYS_MONITOR_INTERNAL_HEAP_WARN_THRESHOLD);
#if (EVENT_BUS_ENABLE == 1)
        system_heap_alert_data_t alert_data = {
            .free_bytes       = info.internal_free,
            .threshold_bytes  = SYS_MONITOR_INTERNAL_HEAP_WARN_THRESHOLD,
            .is_internal_heap = true,
        };
        event_bus_publish_simple(EVENT_TYPE_SYSTEM_ALERT, SYSTEM_ALERT_HEAP_LOW, &alert_data,
                                 sizeof(alert_data));
#endif
    }

    if (info.psram_free < SYS_MONITOR_PSRAM_HEAP_ERROR_THRESHOLD)
    {
        ESP_LOGE(TAG, "PSRAM heap CRITICAL: %u bytes free (threshold: %u)", info.psram_free,
                 SYS_MONITOR_PSRAM_HEAP_ERROR_THRESHOLD);
#if (EVENT_BUS_ENABLE == 1)
        system_heap_alert_data_t alert_data = {
            .free_bytes       = info.psram_free,
            .threshold_bytes  = SYS_MONITOR_PSRAM_HEAP_ERROR_THRESHOLD,
            .is_internal_heap = false,
        };
        event_bus_publish_simple(EVENT_TYPE_SYSTEM_ALERT, SYSTEM_ALERT_HEAP_CRITICAL, &alert_data,
                                 sizeof(alert_data));
#endif
    }
    else if (info.psram_free < SYS_MONITOR_PSRAM_HEAP_WARN_THRESHOLD)
    {
        ESP_LOGW(TAG, "PSRAM heap LOW: %u bytes free (threshold: %u)", info.psram_free,
                 SYS_MONITOR_PSRAM_HEAP_WARN_THRESHOLD);
#if (EVENT_BUS_ENABLE == 1)
        system_heap_alert_data_t alert_data = {
            .free_bytes       = info.psram_free,
            .threshold_bytes  = SYS_MONITOR_PSRAM_HEAP_WARN_THRESHOLD,
            .is_internal_heap = false,
        };
        event_bus_publish_simple(EVENT_TYPE_SYSTEM_ALERT, SYSTEM_ALERT_HEAP_LOW, &alert_data,
                                 sizeof(alert_data));
#endif
    }
}

esp_err_t sys_monitor_init(void)
{
    if (s_initialized)
    {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = alert_timer_callback,
        .name     = "sys_monitor_alert",
    };

    esp_err_t ret = esp_timer_create(&timer_args, &s_alert_timer);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create alert timer: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_timer_start_periodic(s_alert_timer, SYS_MONITOR_ALERT_CHECK_INTERVAL_MS * 1000);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start alert timer: %s", esp_err_to_name(ret));
        esp_timer_delete(s_alert_timer);
        s_alert_timer = NULL;
        return ret;
    }

#if (SYS_MONITOR_ENABLE_CONSOLE_COMMANDS == 1)
    ret = sys_monitor_register_console_commands();
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to register console commands: %s", esp_err_to_name(ret));
    }
#endif

    s_initialized = true;
    ESP_LOGI(TAG, "Initialized (alert interval: %d ms)", SYS_MONITOR_ALERT_CHECK_INTERVAL_MS);
    return ESP_OK;
}

esp_err_t sys_monitor_deinit(void)
{
    if (!s_initialized)
    {
        ESP_LOGW(TAG, "Not initialized");
        return ESP_OK;
    }

    if (s_alert_timer != NULL)
    {
        esp_timer_stop(s_alert_timer);
        esp_timer_delete(s_alert_timer);
        s_alert_timer = NULL;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "Deinitialized");
    return ESP_OK;
}

esp_err_t sys_monitor_get_heap_info(sys_monitor_heap_info_t* info)
{
    if (info == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    heap_info_fill(info);
    return ESP_OK;
}

esp_err_t sys_monitor_get_task_info(sys_monitor_task_info_t* info_array, uint32_t max_count,
                                    uint32_t* actual_count)
{
    if (info_array == NULL || actual_count == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    TaskStatus_t* task_array = calloc(max_count, sizeof(TaskStatus_t));
    if (task_array == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate task array");
        return ESP_ERR_NO_MEM;
    }

    uint32_t    total_runtime;
    UBaseType_t count = uxTaskGetSystemState(task_array, max_count, &total_runtime);

    if (total_runtime == 0)
    {
        total_runtime = 1;
    }

    uint32_t fill_count = (count < max_count) ? count : max_count;
    for (uint32_t i = 0; i < fill_count; i++)
    {
        strncpy(info_array[i].name, task_array[i].pcTaskName, sizeof(info_array[i].name) - 1);
        info_array[i].name[sizeof(info_array[i].name) - 1] = '\0';
        info_array[i].runtime_percent =
            (uint32_t)((task_array[i].ulRunTimeCounter * 100) / total_runtime);
        info_array[i].absolute_runtime = (uint32_t)task_array[i].ulRunTimeCounter;
        info_array[i].stack_high_water_mark =
            task_array[i].usStackHighWaterMark * sizeof(StackType_t);
        info_array[i].current_priority = task_array[i].uxCurrentPriority;
        info_array[i].task_number      = task_array[i].xTaskNumber;
    }

    *actual_count = fill_count;
    free(task_array);
    return ESP_OK;
}

esp_err_t sys_monitor_get_stats(sys_monitor_stats_t* stats)
{
    if (stats == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    heap_info_fill(&stats->heap);
    stats->task_count = uxTaskGetNumberOfTasks();
    stats->uptime_ms  = (uint32_t)(esp_timer_get_time() / 1000);
    return ESP_OK;
}

void sys_monitor_print_heap_info(void)
{
    sys_monitor_heap_info_t info;
    heap_info_fill(&info);

    ESP_LOGI(TAG, "========== Heap Memory Info ==========");
    ESP_LOGI(TAG, "  Total Free:        %8u bytes", info.total_free);
    ESP_LOGI(TAG, "  --- Internal ---");
    ESP_LOGI(TAG, "    Free:            %8u bytes", info.internal_free);
    ESP_LOGI(TAG, "    Largest Block:   %8u bytes", info.internal_largest_free_block);
    ESP_LOGI(TAG, "    Min Ever Free:   %8u bytes", info.internal_min_ever_free);
    ESP_LOGI(TAG, "  --- PSRAM ---");
    ESP_LOGI(TAG, "    Free:            %8u bytes", info.psram_free);
    ESP_LOGI(TAG, "    Largest Block:   %8u bytes", info.psram_largest_free_block);
    ESP_LOGI(TAG, "    Min Ever Free:   %8u bytes", info.psram_min_ever_free);
    ESP_LOGI(TAG, "=======================================");
}

void sys_monitor_print_task_stats(void)
{
    uint32_t      num_tasks  = uxTaskGetNumberOfTasks();
    TaskStatus_t* task_array = calloc(num_tasks, sizeof(TaskStatus_t));
    if (task_array == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate task array");
        return;
    }

    uint32_t    total_runtime;
    UBaseType_t count = uxTaskGetSystemState(task_array, num_tasks, &total_runtime);

    if (total_runtime == 0)
    {
        total_runtime = 1;
    }

    ESP_LOGI(TAG, "========== Task CPU Usage ==========");
    ESP_LOGI(TAG, "  %-16s  %5s  %8s  %8s", "Name", "CPU%", "Runtime", "Priority");
    ESP_LOGI(TAG, "  ----------------  -----  --------  --------");

    for (UBaseType_t i = 0; i < count; i++)
    {
        uint32_t cpu_percent = (uint32_t)((task_array[i].ulRunTimeCounter * 100) / total_runtime);
        ESP_LOGI(TAG, "  %-16s  %4u%%  %8u  %8u", task_array[i].pcTaskName, cpu_percent,
                 (uint32_t)task_array[i].ulRunTimeCounter, task_array[i].uxCurrentPriority);
    }

    ESP_LOGI(TAG, "  Total tasks: %u, Total runtime: %u", (unsigned)count, (unsigned)total_runtime);
    ESP_LOGI(TAG, "=====================================");

    free(task_array);
}

void sys_monitor_print_stack_info(void)
{
    uint32_t      num_tasks  = uxTaskGetNumberOfTasks();
    TaskStatus_t* task_array = calloc(num_tasks, sizeof(TaskStatus_t));
    if (task_array == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate task array");
        return;
    }

    uint32_t    total_runtime;
    UBaseType_t count = uxTaskGetSystemState(task_array, num_tasks, &total_runtime);

    ESP_LOGI(TAG, "========== Task Stack Usage ==========");
    ESP_LOGI(TAG, "  %-16s  %12s  %8s", "Name", "Stack HWM", "Priority");
    ESP_LOGI(TAG, "  ----------------  ------------  --------");

    for (UBaseType_t i = 0; i < count; i++)
    {
        uint32_t stack_hwm = task_array[i].usStackHighWaterMark * sizeof(StackType_t);
        ESP_LOGI(TAG, "  %-16s  %8u bytes  %8u", task_array[i].pcTaskName, stack_hwm,
                 task_array[i].uxCurrentPriority);
    }

    ESP_LOGI(TAG, "=======================================");

    free(task_array);
}

void sys_monitor_print_all_stats(void)
{
    sys_monitor_stats_t stats;
    sys_monitor_get_stats(&stats);

    ESP_LOGI(TAG, "========== System Stats ==========");
    ESP_LOGI(TAG, "  Uptime: %u ms", stats.uptime_ms);
    ESP_LOGI(TAG, "  Tasks:  %u", stats.task_count);
    ESP_LOGI(TAG, "===================================");

    sys_monitor_print_heap_info();
    sys_monitor_print_task_stats();
    sys_monitor_print_stack_info();
}

#if (SYS_MONITOR_ENABLE_CONSOLE_COMMANDS == 1)

static int cmd_sys_heap(int argc, char** argv)
{
    sys_monitor_print_heap_info();
    return 0;
}

static int cmd_sys_tasks(int argc, char** argv)
{
    sys_monitor_print_task_stats();
    return 0;
}

static int cmd_sys_stack(int argc, char** argv)
{
    sys_monitor_print_stack_info();
    return 0;
}

static int cmd_sys_stats(int argc, char** argv)
{
    sys_monitor_print_all_stats();
    return 0;
}

static int cmd_monitor(int argc, char** argv)
{
    if (argc < 2)
    {
        ESP_LOGI(TAG, "Usage: monitor <heap|tasks|stack|all|watch> [interval_sec]");
        ESP_LOGI(TAG, "  heap   - Show heap memory info (internal + PSRAM)");
        ESP_LOGI(TAG, "  tasks  - Show task CPU usage statistics");
        ESP_LOGI(TAG, "  stack  - Show task stack high water mark");
        ESP_LOGI(TAG, "  all    - Show all system stats (heap + tasks + stack)");
        ESP_LOGI(TAG, "  watch  - Periodically show all stats (default interval: 5s)");
        return 0;
    }

    const char* subcmd = argv[1];

    if (strcmp(subcmd, "heap") == 0)
    {
        sys_monitor_print_heap_info();
    }
    else if (strcmp(subcmd, "tasks") == 0)
    {
        sys_monitor_print_task_stats();
    }
    else if (strcmp(subcmd, "stack") == 0)
    {
        sys_monitor_print_stack_info();
    }
    else if (strcmp(subcmd, "all") == 0)
    {
        sys_monitor_print_all_stats();
    }
    else if (strcmp(subcmd, "watch") == 0)
    {
        int interval = 5;
        if (argc >= 3)
        {
            interval = atoi(argv[2]);
            if (interval < 1)
            {
                interval = 1;
            }
            if (interval > 3600)
            {
                interval = 3600;
            }
        }
        ESP_LOGI(TAG, "Starting monitor watch (interval: %ds, press Ctrl+C to stop)", interval);
        while (true)
        {
            sys_monitor_print_all_stats();
            vTaskDelay(pdMS_TO_TICKS(interval * 1000));
        }
    }
    else
    {
        ESP_LOGE(TAG, "Unknown subcommand: %s", subcmd);
        ESP_LOGI(TAG, "Run 'monitor' without arguments for usage info");
        return 1;
    }

    return 0;
}

esp_err_t sys_monitor_register_console_commands(void)
{
    const esp_console_cmd_t monitor_cmd = {
        .command = "monitor",
        .help    = "Unified system monitor (monitor <heap|tasks|stack|all|watch> [sec])",
        .func    = &cmd_monitor,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&monitor_cmd));

    const esp_console_cmd_t heap_cmd = {
        .command = "sys_heap",
        .help    = "Show heap memory info (internal + PSRAM) [alias: monitor heap]",
        .func    = &cmd_sys_heap,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&heap_cmd));

    const esp_console_cmd_t tasks_cmd = {
        .command = "sys_tasks",
        .help    = "Show task CPU usage statistics [alias: monitor tasks]",
        .func    = &cmd_sys_tasks,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&tasks_cmd));

    const esp_console_cmd_t stack_cmd = {
        .command = "sys_stack",
        .help    = "Show task stack high water mark [alias: monitor stack]",
        .func    = &cmd_sys_stack,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&stack_cmd));

    const esp_console_cmd_t stats_cmd = {
        .command = "sys_stats",
        .help    = "Show all system stats (heap + tasks + stack) [alias: monitor all]",
        .func    = &cmd_sys_stats,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&stats_cmd));

    ESP_LOGI(TAG, "Console commands registered (monitor + sys_*)");
    return ESP_OK;
}

#endif

#endif
