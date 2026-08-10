/*
 * app_init.c - System initialization implementation for FocusLamp
 */

#include <stdio.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app_init.h"
#include "app_state.h"
#include "app_event_handler.h"

/* ===================== Component Headers ===================== */
#include "event_bus.h"
#include "event_def.h"
#include "data_type.h"
#include "system_config.h"
#include "error_code.h"

/* BSP */
#include "bsp_gpio.h"
#include "bsp_power.h"
#include "bsp_uart.h"
#include "bsp_spi.h"
#include "bsp_adc.h"

/* Drivers */
#include "power_driver.h"
#include "led_driver.h"
#include "touch_driver.h"
#include "servo_driver.h"
#include "radar_driver.h"
/* Services */
#include "power_service.h"
#include "led_service.h"
#include "sensor_service.h"
#include "touch_service.h"
#include "lcd_service.h"
#include "servo_service.h"
#include "arm_service.h"
#include "motion_controller.h"

/* Network / Communication subsystem (WiFi + WebSocket + MCP + connection mgmt) */
#include "network_manager.h"

static const char *TAG = "app_init";

/* ===================== Initialization Step Table ===================== */
typedef esp_err_t (*init_step_fn_t)(void);

typedef struct {
    const char      *name;      /* Step name for logging */
    init_step_fn_t   fn;        /* Function pointer */
    bool             optional;  /* true = failure is non-fatal */
} init_step_t;

/* ===================== Initialization Steps ===================== */

static esp_err_t step_nvs_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs_flash_erase");
        ret = nvs_flash_init();
    }
    return ret;
}

static esp_err_t step_event_loop_init(void)
{
    return esp_event_loop_create_default();
}

static esp_err_t step_bsp_init(void)
{
    esp_err_t ret;

    ret = bsp_gpio_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bsp_gpio_init failed");
        return ret;
    }

    ret = bsp_power_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bsp_power_init failed");
        return ret;
    }

    ret = bsp_uart_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bsp_uart_init failed");
        return ret;
    }

    ESP_LOGI(TAG, "Skipping bsp_spi_init; LCD driver owns SPI2 bus initialization");

    /* I2S/audio_bridge init: DISABLED (no audio hardware) */
    /* bsp_adc_init: DISABLED (ambient light sensor removed — ADC had no
     * other consumers) */

    return ESP_OK;
}

static esp_err_t step_event_bus_init(void)
{
    return event_bus_init();
}

/*
 * 每个 driver 初始化失败只打 warning 并返回 OK，不阻断后续 driver/service 初始化。
 * 这样某个 driver 板块异常不会影响其他板块的初始化。
 */

static esp_err_t step_power_driver_init(void)
{
    esp_err_t ret = power_driver_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "power_driver_init failed: %s (continuing)", esp_err_to_name(ret));
        return ESP_OK;  /* 失败不阻断，返回 OK 让后续 step 继续 */
    }
    return ESP_OK;
}

static esp_err_t step_led_driver_init(void)
{
    esp_err_t ret = led_driver_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "led_driver_init failed: %s (continuing)", esp_err_to_name(ret));
        return ESP_OK;  /* 失败不阻断，返回 OK 让后续 step 继续 */
    }
    return ESP_OK;
}

static esp_err_t step_touch_driver_init(void)
{
    esp_err_t ret = touch_driver_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "touch_driver_init failed: %s (continuing)", esp_err_to_name(ret));
        return ESP_OK;  /* 失败不阻断，返回 OK 让后续 step 继续 */
    }
    return ESP_OK;
}

static esp_err_t step_servo_driver_init(void)
{
    esp_err_t ret = servo_driver_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "servo_driver_init failed: %s (continuing)", esp_err_to_name(ret));
        return ESP_OK;  /* 失败不阻断，返回 OK 让后续 step 继续 */
    }
    return ESP_OK;
}

static esp_err_t step_radar_driver_init(void)
{
    esp_err_t ret = radar_driver_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "radar_driver_init failed: %s (continuing)", esp_err_to_name(ret));
        return ESP_OK;  /* 失败不阻断，返回 OK 让后续 step 继续 */
    }
    return ESP_OK;
}

/*
 * 每个 service 初始化失败只打 warning 并返回 OK，不阻断后续 service 初始化。
 * 这样某个 service 板块异常不会影响其他板块的初始化。
 */

static esp_err_t step_power_service_init(void)
{
    esp_err_t ret = power_service_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "power_service_init failed: %s (continuing)", esp_err_to_name(ret));
        return ESP_OK;  /* 失败不阻断，返回 OK 让后续 step 继续 */
    }
    return ESP_OK;
}

static esp_err_t step_led_service_init(void)
{
    esp_err_t ret = led_service_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "led_service_init failed: %s (continuing)", esp_err_to_name(ret));
        return ESP_OK;  /* 失败不阻断，返回 OK 让后续 step 继续 */
    }
    return ESP_OK;
}

static esp_err_t step_lcd_service_init(void)
{
    esp_err_t ret = lcd_service_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "lcd_service_init failed: %s (continuing)", esp_err_to_name(ret));
        return ESP_OK;  /* 失败不阻断，返回 OK 让后续 step 继续 */
    }
    return ESP_OK;
}

static esp_err_t step_touch_service_init(void)
{
    esp_err_t ret = touch_service_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "touch_service_init failed: %s (continuing)", esp_err_to_name(ret));
        return ESP_OK;  /* 失败不阻断，返回 OK 让后续 step 继续 */
    }
    return ESP_OK;
}

/* servo_service 须尽早初始化，以启用 legacy servo API（s_em3_initialized / s_lx_initialized）。
 * 放在 sensor_service 之前：sensor 硬件故障不得阻塞舵机运动。 */
static esp_err_t step_servo_service_init(void)
{
    esp_err_t ret = servo_service_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "servo_service_init failed: %s (continuing)", esp_err_to_name(ret));
        return ESP_OK;  /* 失败不阻断，返回 OK 让后续 step 继续 */
    }
    return ESP_OK;
}

static esp_err_t step_sensor_service_init(void)
{
    esp_err_t ret = sensor_service_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "sensor_service_init failed: %s (continuing)", esp_err_to_name(ret));
        return ESP_OK;  /* 失败不阻断，返回 OK 让后续 step 继续 */
    }
    return ESP_OK;
}

static esp_err_t step_arm_service_init(void)
{
    esp_err_t ret = arm_service_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "arm_service_init failed: %s (continuing)", esp_err_to_name(ret));
        return ESP_OK;  /* 失败不阻断，返回 OK 让后续 step 继续 */
    }
    return ESP_OK;
}

static esp_err_t step_motion_controller_init(void)
{
    esp_err_t ret = motion_controller_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "motion_controller_init failed: %s (continuing)", esp_err_to_name(ret));
        return ESP_OK;
    }
    return ESP_OK;
}

/* ===================== Network / Communication Subsystem Init =====================
 * Single entry point that brings up the full communication stack (mirrors
 * Project 1 / FocusLamp_DEMO2):
 *   WiFi -> WebSocket server (/ws + /mcp) -> MCP handler -> Connection manager
 *        -> Heartbeat service -> Message queue
 *
 * Replaces the former separate steps: wifi_manager + wifi_comm_module + mcp_handler.
 * The legacy TCP wifi_comm_module path has been retired in favor of WebSocket.
 * Must run AFTER all hardware services since app_mcp_handler dispatches to them. */
static esp_err_t step_network_manager_init(void)
{
    esp_err_t ret = network_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "network_manager_init failed: %s (continuing)", esp_err_to_name(ret));
        return ESP_OK;  /* 失败不阻断，返回 OK 让后续 step 继续 */
    }
    return ESP_OK;
}

/* ===================== Application Module Init ===================== */
#include "exercise_follow_app.h"
#include "custom_rule_app.h"
#include "focus_app.h"
#include "companion_app.h"
#include "voice_app.h"
#include "lighting_app.h"
#include "game_app.h"
#include "music_rhythm_app.h"
#include "arm_action_app.h"
#include "demo_fake_detection.h"

static esp_err_t step_apps_init(void)
{
    esp_err_t ret;

    ret = exercise_follow_app_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "exercise_follow_app_init failed (optional): %s", esp_err_to_name(ret));
    }

    ret = custom_rule_app_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "custom_rule_app_init failed (optional): %s", esp_err_to_name(ret));
    }

    ret = focus_app_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "focus_app_init failed (optional): %s", esp_err_to_name(ret));
    }

    ret = companion_app_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "companion_app_init failed (optional): %s", esp_err_to_name(ret));
    }

    ret = voice_app_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "voice_app_init failed (optional): %s", esp_err_to_name(ret));
    }

    ret = lighting_app_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "lighting_app_init failed (optional): %s", esp_err_to_name(ret));
    }

    ret = game_app_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "game_app_init failed (optional): %s", esp_err_to_name(ret));
    }

    ret = music_rhythm_app_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "music_rhythm_app_init failed (optional): %s", esp_err_to_name(ret));
    }

    ret = arm_action_app_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "arm_action_app_init failed (optional): %s", esp_err_to_name(ret));
    }

#if CONFIG_PROJECT_DEMO_FAKE_DETECTION_ENABLE
    ret = demo_fake_detection_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "demo_fake_detection_init failed (optional): %s", esp_err_to_name(ret));
    }
#endif /* CONFIG_PROJECT_DEMO_FAKE_DETECTION_ENABLE */

    return ESP_OK;
}

/* ===================== Init Step Table ===================== */

static const init_step_t s_init_steps[] = {
    { "NVS Flash",              step_nvs_init,                 false },
    { "Event Loop",             step_event_loop_init,          false },
    { "BSP",                    step_bsp_init,                 false },
    { "Event Bus",              step_event_bus_init,           false },
    { "power_driver",           step_power_driver_init,        true  },
    { "led_driver",             step_led_driver_init,          true  },
    { "touch_driver",           step_touch_driver_init,        true  },
    { "servo_driver",           step_servo_driver_init,        true  },
    { "radar_driver",           step_radar_driver_init,        true  },
    { "power_service",          step_power_service_init,       true  },
    { "led_service",            step_led_service_init,         true  },
    { "lcd_service",            step_lcd_service_init,         true  },
    { "touch_service",          step_touch_service_init,       true  },
    { "servo_service",          step_servo_service_init,       true  },
    { "sensor_service",         step_sensor_service_init,      true  },
    { "arm_service",            step_arm_service_init,         true  },
    { "motion_controller",      step_motion_controller_init,   true  },
    { "network_manager",        step_network_manager_init,     true  },
    { "Apps",                   step_apps_init,                true  },
};

static const int s_init_step_count = sizeof(s_init_steps) / sizeof(s_init_steps[0]);

/* ===================== Public API ===================== */

void app_init(void)
{
    esp_err_t ret;
    int failed_steps = 0;

    ESP_LOGI(TAG, "=== System Initialization Start ===");
    ESP_LOGI(TAG, "Project: %s v%d.%d.%d, HW: %s",
             PROJECT_NAME,
             SYSTEM_VERSION_MAJOR, SYSTEM_VERSION_MINOR, SYSTEM_VERSION_PATCH,
             HARDWARE_VERSION);

    for (int i = 0; i < s_init_step_count; i++) {
        uint32_t heap_before = esp_get_free_heap_size();
        ESP_LOGI(TAG, "[%d/%d] Initializing: %s ...", i + 1, s_init_step_count, s_init_steps[i].name);

        ret = s_init_steps[i].fn();

        uint32_t heap_after = esp_get_free_heap_size();
        int32_t heap_diff = (int32_t)heap_before - (int32_t)heap_after;

        if (ret != ESP_OK) {
            if (s_init_steps[i].optional) {
                ESP_LOGW(TAG, "Step '%s' failed (optional): %s (heap change: %+ld bytes)",
                         s_init_steps[i].name, esp_err_to_name(ret), heap_diff);
            } else {
                ESP_LOGE(TAG, "Step '%s' failed: %s (heap change: %+ld bytes)",
                         s_init_steps[i].name, esp_err_to_name(ret), heap_diff);
                failed_steps++;
            }
        } else {
            ESP_LOGI(TAG, "Step '%s' completed (heap change: %+ld bytes)", s_init_steps[i].name, heap_diff);
        }
    }

    /* Initialize state manager */
    ret = app_state_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "app_state_manager_init failed: %s", esp_err_to_name(ret));
        failed_steps++;
    }

    /* Register global event handlers */
    ret = app_event_handler_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "app_event_handler_init failed: %s", esp_err_to_name(ret));
        failed_steps++;
    }

    if (failed_steps > 0) {
        ESP_LOGW(TAG, "Initialization completed with %d non-optional failure(s)", failed_steps);
    } else {
        ESP_LOGI(TAG, "=== System Initialization Complete ===");
    }

    /* Publish startup complete event to transition state machine from INIT to IDLE.
     * This is CRITICAL: the handler on_sys_startup_complete sets APP_STATE_IDLE,
     * starts lighting_app, and sets LCD expression to NORMAL.
     * Without this event, the system remains stuck in APP_STATE_INIT forever. */
    event_bus_publish_simple(EV_SYS_STARTUP_COMPLETE);
}