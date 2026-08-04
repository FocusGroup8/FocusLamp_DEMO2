#ifndef SYS_MONITOR_H
#define SYS_MONITOR_H

#include "esp_err.h"

#include "sys_monitor_config.h"
#include "sys_monitor_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

#if (SYS_MONITOR_ENABLE == 1)

    esp_err_t sys_monitor_init(void);
    esp_err_t sys_monitor_deinit(void);

    esp_err_t sys_monitor_get_heap_info(sys_monitor_heap_info_t* info);
    esp_err_t sys_monitor_get_task_info(sys_monitor_task_info_t* info_array, uint32_t max_count,
                                        uint32_t* actual_count);
    esp_err_t sys_monitor_get_stats(sys_monitor_stats_t* stats);

    void sys_monitor_print_heap_info(void);
    void sys_monitor_print_task_stats(void);
    void sys_monitor_print_stack_info(void);
    void sys_monitor_print_all_stats(void);

    esp_err_t sys_monitor_register_console_commands(void);

#else

static inline esp_err_t sys_monitor_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t sys_monitor_deinit(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t sys_monitor_get_heap_info(sys_monitor_heap_info_t* info)
{
    (void)info;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t sys_monitor_get_task_info(sys_monitor_task_info_t* info_array,
                                                  uint32_t max_count, uint32_t* actual_count)
{
    (void)info_array;
    (void)max_count;
    (void)actual_count;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t sys_monitor_get_stats(sys_monitor_stats_t* stats)
{
    (void)stats;
    return ESP_ERR_NOT_SUPPORTED;
}

static inline void sys_monitor_print_heap_info(void)
{
}
static inline void sys_monitor_print_task_stats(void)
{
}
static inline void sys_monitor_print_stack_info(void)
{
}
static inline void sys_monitor_print_all_stats(void)
{
}

static inline esp_err_t sys_monitor_register_console_commands(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif

#ifdef __cplusplus
}
#endif

#endif
