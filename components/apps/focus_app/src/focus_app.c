/*
 * focus_app.c - Focus timer application implementation
 */

#include "focus_app.h"
#include "event_bus.h"
#include "event_def.h"
#include "led_service.h"
#include "lcd_service.h"
#include "audio_service.h"
#include "lamp_head_controller.h"
#include "tts_bridge.h"
#include "data_type.h"
#include "device_state.h"
#include "error_code.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "focus_app";

/* ===================== Private Data ===================== */
static bool s_initialized = false;
static bool s_running = false;
static bool s_paused = false;

static esp_timer_handle_t s_focus_timer = NULL;
static uint32_t s_duration_sec = 0;
static uint32_t s_elapsed_sec = 0;

/* 在位久坐提醒状态：雷达在场连续累计，每会话最多播报1次 */
#define FOCUS_SEATED_REMIND_SEC 60
static uint32_t s_seated_sec = 0;
static bool s_seated_notified = false;

/* 灯头当前亮度百分比，用于检测环境光档位变化后刷新 */
static uint8_t s_last_head_pct = 0;

/* ===================== Internal Helpers ===================== */
static void focus_app_apply_lighting(void)
{
    /* 底部灯光关闭 */
    led_service_turn_off();

    /* 头部灯光正常显示：亮度档位随环境光同向一一对应 */
    device_state_t state = {0};
    device_state_get(&state);
    uint8_t head_pct = lamp_head_percent_from_ambient(state.ambient_light.level);
    s_last_head_pct  = head_pct;
    lamp_head_led_on(head_pct);

    /* 大屏幕显示正常表情 */
    lamp_head_set_expression("neutral");

    ESP_LOGI(TAG, "Applied focus lighting: base off, head %d%%, expression neutral",
             head_pct);
}

/* 专注模式运行中：环境光档位变化时刷新灯头亮度（同向一一对应）。
 * 由 1s 定时器周期检查，光感平滑发布约 10s/次，实际刷新频率受其约束。 */
static void focus_app_update_lighting(void)
{
    device_state_t state = {0};
    device_state_get(&state);
    uint8_t head_pct = lamp_head_percent_from_ambient(state.ambient_light.level);
    if (head_pct != s_last_head_pct) {
        s_last_head_pct = head_pct;
        ESP_LOGI(TAG, "Ambient level %d -> head %d%%", state.ambient_light.level, head_pct);
        lamp_head_led_on(head_pct);
    }
}

static void focus_app_start_white_noise(void)
{
    esp_err_t ret = audio_service_play("file:///spiffs/audio/white_noise.wav");
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to play white noise file, falling back to tone");
        audio_service_play_tone(200, 0);
    }
    audio_service_set_volume(40);
    ESP_LOGI(TAG, "White noise playback started");
}

static void focus_app_stop_white_noise(void)
{
    audio_service_stop();
    ESP_LOGI(TAG, "White noise playback stopped");
}

static void focus_app_timer_callback(void *arg)
{
    /* 语音对话时暂停倒计时：读取语音板上报的对话状态
     * （语音板经 POST /api/chat/state 写入 device_state.voice_active） */
    device_state_t state = {0};
    device_state_get(&state);

    /* 环境光档位变化时刷新灯头亮度（专注灯光随环境光自适应） */
    focus_app_update_lighting();

    bool voice_active = state.voice_active;

    if (voice_active) {
        if (!s_paused) {
            s_paused = true;
            ESP_LOGI(TAG, "Countdown paused: voice dialogue active");
        }
        event_bus_publish_simple(EV_APP_FOCUS_TIMER_TICK);
        return;
    }

    if (s_paused) {
        s_paused = false;
        ESP_LOGI(TAG, "Countdown resumed: voice dialogue ended");
    }

    s_elapsed_sec++;

    /* 在位连续≥60s（倒计时运行中，即非语音对话）播报久坐提醒 */
    if (state.radar.present) {
        s_seated_sec++;
        if (s_seated_sec >= FOCUS_SEATED_REMIND_SEC && !s_seated_notified) {
            s_seated_notified = true;
            ESP_LOGI(TAG, "User seated >=%d s, broadcasting sedentary reminder",
                     FOCUS_SEATED_REMIND_SEC);
            tts_bridge_speak("久坐提醒");
        }
    } else {
        s_seated_sec = 0;
    }

    /* Publish tick event for UI updates */
    event_bus_publish_simple(EV_APP_FOCUS_TIMER_TICK);

    if (s_elapsed_sec >= s_duration_sec) {
        /* Focus session complete — broadcast end notice via short command
         * (cloud maps "专注结束" to the full text, synced manually by user). */
        ESP_LOGI(TAG, "Focus session complete, broadcasting end notice");
        tts_bridge_speak("专注结束");
        event_bus_publish_simple(EV_APP_FOCUS_TIMER_DONE);
        focus_app_stop();
        ESP_LOGI(TAG, "Focus session complete");
    }
}

/* ===================== Event Handlers ===================== */
static void focus_app_on_mode_changed(event_t *event, void *context)
{
    if (event == NULL || event->data == NULL) {
        return;
    }
    app_state_t new_state = *(app_state_t *)event->data;
    if (new_state != APP_STATE_FOCUS && s_running) {
        focus_app_stop();
    }
}

/* ===================== Public API ===================== */
esp_err_t focus_app_init(void)
{
    if (s_initialized) {
        return ERR_ALREADY_INITIALIZED;
    }

    esp_err_t ret = event_bus_subscribe(EV_APP_MODE_CHANGED, focus_app_on_mode_changed, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to EV_APP_MODE_CHANGED");
        return ret;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = focus_app_timer_callback,
        .arg = NULL,
        .name = "focus_timer",
        .dispatch_method = ESP_TIMER_TASK,
    };
    ret = esp_timer_create(&timer_args, &s_focus_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create focus timer");
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Focus app initialized");
    return ESP_OK;
}

esp_err_t focus_app_start(uint32_t duration_minutes)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    if (s_running) {
        return ERR_BUSY;
    }

    s_duration_sec = duration_minutes * 60;
    s_elapsed_sec = 0;
    s_paused = false;
    s_running = true;
    s_seated_sec = 0;
    s_seated_notified = false;

    /* Apply focus environment */
    focus_app_apply_lighting();
    focus_app_start_white_noise();

    /* 小屏幕显示专注模式（标题留空避免重复，进度条+倒计时由 lcd_service 内部管理） */
    lcd_service_task_start("", s_duration_sec);
    lcd_service_page_switch_to(LCD_PAGE_INFO);

    /* Start timer (1 second period) */
    esp_timer_start_periodic(s_focus_timer, 1000000);

    event_bus_publish_simple(EV_APP_FOCUS_TIMER_START);

    /* TTS播报：专注模式已开启 */
    tts_bridge_speak("专注开启");

    ESP_LOGI(TAG, "Focus session started: %u minutes", duration_minutes);
    return ESP_OK;
}

esp_err_t focus_app_stop(void)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    if (!s_running) {
        return ERR_BUSY;
    }

    if (s_focus_timer != NULL) {
        esp_timer_stop(s_focus_timer);
    }

    focus_app_stop_white_noise();

    /* 关闭头部灯光 */
    lamp_head_led_off();
    lamp_head_set_expression("neutral");

    led_service_turn_off();
    lcd_service_task_stop();
    lcd_service_page_switch_to(LCD_PAGE_EXPRESSION);

    s_running = false;
    s_paused = false;
    s_elapsed_sec = 0;
    s_last_head_pct = 0;

    event_bus_publish_simple(EV_APP_STATE_CHANGED);
    ESP_LOGI(TAG, "Focus session stopped");
    return ESP_OK;
}

esp_err_t focus_app_pause(void)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    if (!s_running || s_paused) {
        return ERR_BUSY;
    }

    if (s_focus_timer != NULL) {
        esp_timer_stop(s_focus_timer);
    }
    s_paused = true;

    event_bus_publish_simple(EV_APP_STATE_CHANGED);
    ESP_LOGI(TAG, "Focus session paused");
    return ESP_OK;
}

esp_err_t focus_app_resume(void)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    if (!s_running || !s_paused) {
        return ERR_BUSY;
    }

    if (s_focus_timer != NULL) {
        esp_timer_start_periodic(s_focus_timer, 1000000);
    }
    s_paused = false;

    event_bus_publish_simple(EV_APP_STATE_CHANGED);
    ESP_LOGI(TAG, "Focus session resumed");
    return ESP_OK;
}

esp_err_t focus_app_get_remaining(uint32_t *remaining_sec)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    if (remaining_sec == NULL) {
        return ERR_INVALID_PARAM;
    }
    *remaining_sec = (s_elapsed_sec < s_duration_sec) ? (s_duration_sec - s_elapsed_sec) : 0;
    return ESP_OK;
}