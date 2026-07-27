/*
 * voice_app.c - Voice control application implementation
 */

#include "voice_app.h"
#include "event_bus.h"
#include "event_def.h"
#include "led_service.h"
#include "lcd_service.h"
#include "arm_service.h"
#include "arm_action_app.h"
#include "audio_service.h"
#include "xiaozhi_manager.h"
#include "data_type.h"
#include "device_state.h"
#include "error_code.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "voice_app";

/* ===================== Private Data ===================== */
static bool s_initialized = false;
static bool s_running = false;

/* ===================== Command Parsing ===================== */
/* NOTE: Integrate with actual voice recognition SDK for NLU */

typedef struct {
    const char *keyword;
    esp_err_t (*handler)(void);
} voice_command_t;

/* Forward declarations of command handlers */
static esp_err_t cmd_light_on(void);
static esp_err_t cmd_light_off(void);
static esp_err_t cmd_brightness_up(void);
static esp_err_t cmd_brightness_down(void);
static esp_err_t cmd_colortemp_warm(void);
static esp_err_t cmd_colortemp_cool(void);
static esp_err_t cmd_mode_focus(void);
static esp_err_t cmd_mode_companion(void);
static esp_err_t cmd_mode_music(void);
static esp_err_t cmd_arm_wave(void);
static esp_err_t cmd_arm_stop(void);

/* Command keyword table */
static const voice_command_t s_commands[] = {
    /* Light control */
    { "open light",      cmd_light_on        },
    { "turn on light",   cmd_light_on        },
    { "close light",     cmd_light_off       },
    { "turn off light",  cmd_light_off       },
    /* Brightness */
    { "brightness up",   cmd_brightness_up   },
    { "brighter",        cmd_brightness_up   },
    { "brightness down", cmd_brightness_down },
    { "dimmer",          cmd_brightness_down },
    /* Color temperature */
    { "warm light",      cmd_colortemp_warm  },
    { "cool light",      cmd_colortemp_cool  },
    /* Mode switching */
    { "focus mode",      cmd_mode_focus      },
    { "companion mode",  cmd_mode_companion  },
    { "music mode",      cmd_mode_music      },
    /* Arm actions */
    { "wave",            cmd_arm_wave        },
    { "stop arm",        cmd_arm_stop        },
};

static const size_t s_command_count = sizeof(s_commands) / sizeof(s_commands[0]);

/* ===================== Command Handlers ===================== */
static esp_err_t cmd_light_on(void)
{
    led_service_set_brightness(200);
    ESP_LOGI(TAG, "Command: light on");
    return ESP_OK;
}

static esp_err_t cmd_light_off(void)
{
    led_service_set_brightness(0);
    ESP_LOGI(TAG, "Command: light off");
    return ESP_OK;
}

static esp_err_t cmd_brightness_up(void)
{
    led_service_brightness_up();
    ESP_LOGI(TAG, "Command: brightness up");
    return ESP_OK;
}

static esp_err_t cmd_brightness_down(void)
{
    led_service_brightness_down();
    ESP_LOGI(TAG, "Command: brightness down");
    return ESP_OK;
}

static esp_err_t cmd_colortemp_warm(void)
{
    led_service_set_mode(LED_MODE_WARM);
    ESP_LOGI(TAG, "Command: warm color temperature");
    return ESP_OK;
}

static esp_err_t cmd_colortemp_cool(void)
{
    led_service_set_mode(LED_MODE_WHITE);
    ESP_LOGI(TAG, "Command: cool color temperature");
    return ESP_OK;
}

static esp_err_t cmd_mode_focus(void)
{
    app_mode_t mode = APP_MODE_FOCUS;
    event_t ev = {
        .type = EV_APP_MODE_CHANGED,
        .data = &mode,
        .data_size = sizeof(mode),
        .timestamp = event_bus_get_timestamp(),
    };
    event_bus_publish(&ev);
    ESP_LOGI(TAG, "Command: switch to focus mode");
    return ESP_OK;
}

static esp_err_t cmd_mode_companion(void)
{
    app_mode_t mode = APP_MODE_COMPANION;
    event_t ev = {
        .type = EV_APP_MODE_CHANGED,
        .data = &mode,
        .data_size = sizeof(mode),
        .timestamp = event_bus_get_timestamp(),
    };
    event_bus_publish(&ev);
    ESP_LOGI(TAG, "Command: switch to companion mode");
    return ESP_OK;
}

static esp_err_t cmd_mode_music(void)
{
    app_mode_t mode = APP_MODE_MUSIC_RHYTHM;
    event_t ev = {
        .type = EV_APP_MODE_CHANGED,
        .data = &mode,
        .data_size = sizeof(mode),
        .timestamp = event_bus_get_timestamp(),
    };
    event_bus_publish(&ev);
    ESP_LOGI(TAG, "Command: switch to music rhythm mode");
    return ESP_OK;
}

static esp_err_t cmd_arm_wave(void)
{
    arm_action_app_play(ARM_ACTION_WAVE);
    ESP_LOGI(TAG, "Command: arm wave");
    return ESP_OK;
}

static esp_err_t cmd_arm_stop(void)
{
    arm_service_stop_action();
    ESP_LOGI(TAG, "Command: arm stop");
    return ESP_OK;
}

/* ===================== Event Handlers ===================== */
static void voice_app_on_mode_changed(event_t *event, void *context)
{
    (void)context;
    if (event == NULL || event->data == NULL || event->data_size != sizeof(uint8_t) * 2) {
        return;
    }
    const uint8_t *state_data = (const uint8_t *)event->data;
    app_state_t new_state = (app_state_t)state_data[1];
    if (new_state != APP_STATE_VOICE && s_running) {
        voice_app_stop();
    }
}

static void voice_app_on_voice_result(event_t *event, void *context)
{
    (void)context;
    if (event == NULL || event->data == NULL || event->data_size == 0 || !s_running) {
        return;
    }
    const char *text = (const char *)event->data;
    voice_app_process_command(text);
}

/* ===================== Public API ===================== */
esp_err_t voice_app_init(void)
{
    if (s_initialized) {
        return ERR_ALREADY_INITIALIZED;
    }

    esp_err_t ret = event_bus_subscribe(EV_APP_MODE_CHANGED, voice_app_on_mode_changed, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to EV_APP_MODE_CHANGED");
        return ret;
    }

    ret = event_bus_subscribe(EV_VOICE_COMMAND_RECOGNIZED, voice_app_on_voice_result, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to EV_VOICE_COMMAND_RECOGNIZED");
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Voice app initialized");
    return ESP_OK;
}

esp_err_t voice_app_start(void)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    if (s_running) {
        return ERR_BUSY;
    }

    s_running = true;

    /* 打开小智音频通道（麦克风上传 + TTS 下行），让用户可与云端语音助手对话。
     * 失败则退化为离线模式（本地关键词匹配，需本地 STT 输入）。 */
    esp_err_t ret = xiaozhi_manager_open_audio_channel();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "xiaozhi audio channel open failed: %s (offline mode)", esp_err_to_name(ret));
    }

    lcd_service_page_switch_to(LCD_PAGE_INFO);

    event_bus_publish_simple(EV_APP_STATE_CHANGED);
    ESP_LOGI(TAG, "Voice app started (xiaozhi audio %s)", ret == ESP_OK ? "open" : "unavailable");
    return ESP_OK;
}

esp_err_t voice_app_stop(void)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    if (!s_running) {
        return ERR_BUSY;
    }

    /* 关闭小智音频通道 */
    xiaozhi_manager_close_audio_channel();
    s_running = false;

    event_bus_publish_simple(EV_APP_STATE_CHANGED);
    ESP_LOGI(TAG, "Voice app stopped");
    return ESP_OK;
}

esp_err_t voice_app_process_command(const char *text)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    if (text == NULL) {
        return ERR_INVALID_PARAM;
    }
    if (!s_running) {
        return ERR_BUSY;
    }

    /* Simple keyword matching (NOTE: replace with NLU engine) */
    for (size_t i = 0; i < s_command_count; i++) {
        if (strstr(text, s_commands[i].keyword) != NULL) {
            ESP_LOGI(TAG, "Matched command: %s", s_commands[i].keyword);
            return s_commands[i].handler();
        }
    }

    ESP_LOGW(TAG, "Unknown command: %s", text);
    return ERR_NOT_FOUND;
}