/*
 * companion_app.c - Companion application implementation
 */

#include "companion_app.h"
#include "event_bus.h"
#include "event_def.h"
#include "led_service.h"
#include "lcd_service.h"
#include "servo_service.h"
#include "arm_service.h"
#include "data_type.h"
#include "device_state.h"
#include "error_code.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "companion_app";

/* ===================== Private Data ===================== */
static bool s_initialized = false;
static bool s_running = false;

static esp_timer_handle_t s_companion_timer = NULL;
static uint8_t s_hue = 0; /* For RGB gradual cycling */

/* ===================== Predefined Companion Action Sequence ===================== */
/* Default companion action sequence; adjust servo positions and timing as needed */
static const action_step_t s_companion_steps[] = {
    { 0, 500,  800, 200 },
    { 1, 600,  800, 200 },
    { 2, 400,  800, 200 },
    { 0, 512, 1000, 300 },
};

static const action_sequence_t s_companion_seq = {
    .steps      = s_companion_steps,
    .step_count = 4,
    .loop_count = 0, /* Infinite loop */
};

/* ===================== Internal Helpers ===================== */
static void hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t s_255 = (uint16_t)s * 255 / 100;
    uint8_t v_255 = (uint16_t)v * 255 / 100;
    uint8_t region = (h / 60) % 6;
    uint8_t remainder = (h % 60) * 255 / 60;

    uint8_t p = (v_255 * (255 - s_255)) / 255;
    uint8_t q = (v_255 * (255 - (s_255 * remainder) / 255)) / 255;
    uint8_t t = (v_255 * (255 - (s_255 * (255 - remainder)) / 255)) / 255;

    switch (region) {
        case 0: *r = v_255; *g = t; *b = p; break;
        case 1: *r = q; *g = v_255; *b = p; break;
        case 2: *r = p; *g = v_255; *b = t; break;
        case 3: *r = p; *g = q; *b = v_255; break;
        case 4: *r = t; *g = p; *b = v_255; break;
        default: *r = v_255; *g = p; *b = q; break;
    }
}

static void companion_app_update_lighting(void)
{
    /* Gradual RGB color cycling */
    hsv_color_t hsv = {
        .h = s_hue,
        .s = 80,
        .v = 60,
    };
    uint8_t r, g, b;
    hsv_to_rgb(hsv.h, hsv.s, hsv.v, &r, &g, &b);
    led_service_set_color(r, g, b);
    ESP_LOGI(TAG, "Companion lighting: HSV(%u,%u,%u) -> RGB(%u,%u,%u)",
             hsv.h, hsv.s, hsv.v, r, g, b);
    s_hue = (s_hue + 1) % 360;
}

static void companion_app_update_expression(void)
{
    /* Cycle through friendly expressions */
    static lcd_expression_t expressions[] = {
        LCD_EXPRESSION_HAPPY,
        LCD_EXPRESSION_HAPPY,
        LCD_EXPRESSION_SURPRISED,
    };
    static uint8_t idx = 0;
    lcd_service_expression_set(expressions[idx]);
    idx = (idx + 1) % (sizeof(expressions) / sizeof(expressions[0]));
}

static void companion_app_timer_callback(void *arg)
{
    (void)arg;
    if (!s_running) {
        return;
    }
    companion_app_update_lighting();
    companion_app_update_expression();
}

/* ===================== Event Handlers ===================== */
static void companion_app_on_mode_changed(event_t *event, void *context)
{
    if (event == NULL || event->data == NULL) {
        return;
    }
    uint8_t *state_data = (uint8_t *)event->data;
    app_state_t new_state = (app_state_t)state_data[1];
    if (new_state != APP_STATE_COMPANION && s_running) {
        companion_app_stop();
    }
}

/* ===================== Public API ===================== */
esp_err_t companion_app_init(void)
{
    if (s_initialized) {
        return ERR_ALREADY_INITIALIZED;
    }

    esp_err_t ret = event_bus_subscribe(EV_APP_MODE_CHANGED, companion_app_on_mode_changed, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to EV_APP_MODE_CHANGED");
        return ret;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = companion_app_timer_callback,
        .arg = NULL,
        .name = "companion_timer",
        .dispatch_method = ESP_TIMER_TASK,
    };
    ret = esp_timer_create(&timer_args, &s_companion_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create companion timer");
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Companion app initialized");
    return ESP_OK;
}

esp_err_t companion_app_start(void)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    if (s_running) {
        return ERR_BUSY;
    }

    s_hue = 0;
    s_running = true;

    /* Set ambient LED effect */
    led_service_set_mode(LED_MODE_AMBIENT);
    led_service_set_effect(LED_EFFECT_BREATHING);

    /* Show happy expression on LCD */
    lcd_service_expression_set(LCD_EXPRESSION_HAPPY);

    /* Start arm friendly action loop */
    arm_service_load_action(&s_companion_seq);
    arm_service_start_action();

    /* Start periodic timer for lighting updates (100ms) */
    esp_timer_start_periodic(s_companion_timer, 100000);

    event_bus_publish_simple(EV_APP_STATE_CHANGED);
    ESP_LOGI(TAG, "Companion app started");
    return ESP_OK;
}

esp_err_t companion_app_stop(void)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    if (!s_running) {
        return ERR_BUSY;
    }

    if (s_companion_timer != NULL) {
        esp_timer_stop(s_companion_timer);
    }

    /* Stop arm action */
    arm_service_stop_action();

    /* Restore default lighting and LCD */
    led_service_turn_off();
    lcd_service_page_switch_to(LCD_PAGE_EXPRESSION);
    lcd_service_expression_set(LCD_EXPRESSION_NORMAL);

    s_running = false;

    event_bus_publish_simple(EV_APP_STATE_CHANGED);
    ESP_LOGI(TAG, "Companion app stopped");
    return ESP_OK;
}