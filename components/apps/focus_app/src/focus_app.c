/*
 * focus_app.c - Focus timer application implementation
 */

#include "focus_app.h"
#include "event_bus.h"
#include "event_def.h"
#include "led_service.h"
#include "lcd_service.h"
#include "audio_service.h"
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

/* ===================== Internal Helpers ===================== */
static void focus_app_apply_lighting(void)
{
    led_service_set_mode(LED_MODE_WHITE);
    led_service_set_effect(LED_EFFECT_STEADY);
    led_service_set_brightness(180);
    ESP_LOGI(TAG, "Applied focus lighting: cool white, brightness=180");
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
    s_elapsed_sec++;

    /* Publish tick event for UI updates */
    event_bus_publish_simple(EV_APP_FOCUS_TIMER_TICK);

    if (s_elapsed_sec >= s_duration_sec) {
        /* Focus session complete */
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

    /* Apply focus environment */
    focus_app_apply_lighting();
    focus_app_start_white_noise();

    lcd_service_info_set_title("Focus");
    lcd_service_page_switch_to(LCD_PAGE_INFO);

    /* Start timer (1 second period) */
    esp_timer_start_periodic(s_focus_timer, 1000000);

    event_bus_publish_simple(EV_APP_FOCUS_TIMER_START);
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

    led_service_turn_off();
    lcd_service_page_switch_to(LCD_PAGE_EXPRESSION);

    s_running = false;
    s_paused = false;
    s_elapsed_sec = 0;

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