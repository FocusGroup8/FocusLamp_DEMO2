/*
 * custom_rule_app.c - Custom mode rule engine implementation
 * 
 * Monitors triggers (time, sensor, touch) and executes configured actions.
 * Rules are persisted in NVS namespace "custom_rules".
 */

#include "custom_rule_app.h"
#include "event_bus.h"
#include "event_def.h"
#include "led_service.h"
#include "lcd_service.h"
#include "audio_service.h"
#include "arm_service.h"
#include "data_type.h"
#include "device_state.h"
#include "error_code.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "custom_rule_app";

/* ===================== NVS Constants ===================== */
#define NVS_NAMESPACE      "custom_rules"
#define NVS_KEY_RULE_COUNT "rule_count"
#define NVS_KEY_RULE_FMT   "rule_%u"

/* ===================== Private Data ===================== */
static bool s_initialized = false;
static bool s_running = false;

static esp_timer_handle_t s_rule_timer = NULL;
static custom_rule_t s_rules[CUSTOM_RULE_MAX_SLOTS];

/* Track last trigger time for cooldown */
static uint32_t s_last_trigger_time[CUSTOM_RULE_MAX_SLOTS];

/* ===================== Trigger Check Helpers ===================== */
static uint32_t get_time_seconds(void)
{
    return esp_timer_get_time() / 1000000ULL;
}

static bool check_time_trigger(const custom_trigger_t *trigger)
{
    /* Get current local time (simplified: uses system time). 
       In real deployment, would use SNTP-synced time. */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);

    uint8_t cur_hour = (uint8_t)tm_info->tm_hour;
    uint8_t cur_min = (uint8_t)tm_info->tm_min;

    return (cur_hour == trigger->config.time.hour &&
            cur_min == trigger->config.time.minute);
}

static bool check_radar_trigger(const custom_trigger_t *trigger, const event_t *event)
{
    if (event == NULL) return false;

    bool human_present = false;
    if (event->type == EV_RADAR_PRESENCE) {
        human_present = true;
    }

    return (trigger->config.radar.present == human_present);
}

static bool check_touch_trigger(const custom_trigger_t *trigger, const event_t *event)
{
    if (event == NULL) return false;

    /* Map touch point from event type */
    uint8_t touch_point = 0xFF;
    if (event->type >= EV_TOUCH_A_SINGLE_CLICK && event->type <= EV_TOUCH_A_RELEASE) {
        touch_point = 0;
    } else if (event->type >= EV_TOUCH_B_SINGLE_CLICK && event->type <= EV_TOUCH_B_RELEASE) {
        touch_point = 1;
    } else if (event->type >= EV_TOUCH_C_SINGLE_CLICK && event->type <= EV_TOUCH_C_RELEASE) {
        touch_point = 2;
    } else if (event->type >= EV_TOUCH_D_SINGLE_CLICK && event->type <= EV_TOUCH_D_RELEASE) {
        touch_point = 3;
    }

    if (touch_point != trigger->config.touch.touch_point) {
        return false;
    }

    /* Check touch event type */
    custom_trigger_type_t trigger_type = trigger->type;
    if (trigger_type == CUSTOM_TRIGGER_TYPE_TOUCH_SINGLE &&
        (event->type == EV_TOUCH_A_SINGLE_CLICK || event->type == EV_TOUCH_B_SINGLE_CLICK ||
         event->type == EV_TOUCH_C_SINGLE_CLICK || event->type == EV_TOUCH_D_SINGLE_CLICK)) {
        return true;
    }
    if (trigger_type == CUSTOM_TRIGGER_TYPE_TOUCH_DOUBLE &&
        (event->type == EV_TOUCH_A_DOUBLE_CLICK || event->type == EV_TOUCH_B_DOUBLE_CLICK ||
         event->type == EV_TOUCH_C_DOUBLE_CLICK || event->type == EV_TOUCH_D_DOUBLE_CLICK)) {
        return true;
    }
    if (trigger_type == CUSTOM_TRIGGER_TYPE_TOUCH_LONG_PRESS &&
        (event->type == EV_TOUCH_A_LONG_PRESS || event->type == EV_TOUCH_B_LONG_PRESS ||
         event->type == EV_TOUCH_C_LONG_PRESS || event->type == EV_TOUCH_D_LONG_PRESS)) {
        return true;
    }

    return false;
}

/* ===================== Action Execution ===================== */
static esp_err_t execute_action(const custom_action_t *action)
{
    if (action == NULL) {
        return ERR_INVALID_PARAM;
    }

    switch (action->type) {
    case CUSTOM_ACTION_TYPE_ARM_MOVE: {
        action_step_t step = {
            .servo_id    = action->config.arm_move.servo_id,
            .position    = action->config.arm_move.position,
            .duration_ms = action->config.arm_move.duration_ms,
            .delay_ms    = 0,
        };
        action_sequence_t seq = {
            .steps = &step, .step_count = 1, .loop_count = 1,
        };
        arm_service_load_action(&seq);
        arm_service_start_action();
        break;
    }

    case CUSTOM_ACTION_TYPE_LED_MODE:
        led_service_set_mode((led_mode_t)action->config.led.mode);
        led_service_set_brightness(action->config.led.brightness);
        led_service_set_color(action->config.led.r, action->config.led.g, action->config.led.b);
        break;

    case CUSTOM_ACTION_TYPE_LED_EFFECT:
        led_service_set_effect((led_effect_t)action->config.led_effect.effect);
        break;

    case CUSTOM_ACTION_TYPE_LCD_EXPRESSION:
        lcd_service_expression_set((lcd_expression_t)action->config.lcd.expression);
        lcd_service_page_switch_to(LCD_PAGE_EXPRESSION);
        break;

    case CUSTOM_ACTION_TYPE_AUDIO_PLAY:
        audio_service_play(action->config.audio_play.uri);
        break;

    case CUSTOM_ACTION_TYPE_AUDIO_TONE:
        audio_service_play_tone(action->config.audio_tone.freq_hz,
                                action->config.audio_tone.duration_ms);
        break;

    case CUSTOM_ACTION_TYPE_DELAY:
        if (action->config.delay.delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(action->config.delay.delay_ms));
        }
        break;

    default:
        return ERR_UNSUPPORTED;
    }

    return ESP_OK;
}

/* ===================== Rule Execution ===================== */
static void execute_rule(uint8_t slot)
{
    custom_rule_t *rule = &s_rules[slot];

    if (!rule->enabled) {
        return;
    }

    /* Check cooldown */
    uint32_t now = get_time_seconds();
    if (now - s_last_trigger_time[slot] < rule->cooldown_sec) {
        ESP_LOGD(TAG, "Rule '%s' in cooldown", rule->name);
        return;
    }

    ESP_LOGI(TAG, "Triggering rule: %s (%d actions)", rule->name, rule->action_count);

    s_last_trigger_time[slot] = now;

    for (uint8_t i = 0; i < rule->action_count; i++) {
        esp_err_t ret = execute_action(&rule->actions[i]);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Action %d failed: %s", i, esp_err_to_name(ret));
        }
    }
}

/* ===================== Periodic Timer ===================== */
static void rule_timer_callback(void *arg)
{
    (void)arg;
    if (!s_running) return;

    for (uint8_t i = 0; i < CUSTOM_RULE_MAX_SLOTS; i++) {
        if (!s_rules[i].enabled) continue;

        switch (s_rules[i].trigger.type) {
        case CUSTOM_TRIGGER_TYPE_TIME:
            if (check_time_trigger(&s_rules[i].trigger)) {
                execute_rule(i);
            }
            break;

        case CUSTOM_TRIGGER_TYPE_LIGHT_LEVEL:
            /* Light level trigger disabled: the ambient light sensor module
             * has been removed from the hardware. */
            ESP_LOGW(TAG, "Light level trigger skipped (light sensor removed)");
            break;

        default:
            /* Other types handled by event callbacks */
            break;
        }
    }
}

/* ===================== Event Handlers ===================== */
static void custom_rule_on_mode_changed(event_t *event, void *context)
{
    (void)context;
    if (event == NULL || event->data == NULL || event->data_size < sizeof(uint8_t) * 2) {
        return;
    }
    const uint8_t *state_data = (const uint8_t *)event->data;
    app_state_t new_state = (app_state_t)state_data[1];
    if (new_state != APP_STATE_CUSTOM_RULE && s_running) {
        custom_rule_app_stop();
    }
}

static void custom_rule_on_touch_event(event_t *event, void *context)
{
    (void)context;
    if (event == NULL || !s_running) return;

    for (uint8_t i = 0; i < CUSTOM_RULE_MAX_SLOTS; i++) {
        if (!s_rules[i].enabled) continue;

        custom_trigger_type_t t = s_rules[i].trigger.type;
        if (t == CUSTOM_TRIGGER_TYPE_TOUCH_SINGLE ||
            t == CUSTOM_TRIGGER_TYPE_TOUCH_DOUBLE ||
            t == CUSTOM_TRIGGER_TYPE_TOUCH_LONG_PRESS) {
            if (check_touch_trigger(&s_rules[i].trigger, event)) {
                execute_rule(i);
            }
        }
    }
}

static void custom_rule_on_radar_event(event_t *event, void *context)
{
    (void)context;
    if (event == NULL || !s_running) return;

    for (uint8_t i = 0; i < CUSTOM_RULE_MAX_SLOTS; i++) {
        if (!s_rules[i].enabled) continue;

        custom_trigger_type_t t = s_rules[i].trigger.type;
        if (t == CUSTOM_TRIGGER_TYPE_RADAR_PRESENCE ||
            t == CUSTOM_TRIGGER_TYPE_RADAR_CLEAR) {
            if (check_radar_trigger(&s_rules[i].trigger, event)) {
                execute_rule(i);
            }
        }
    }
}

/* ===================== NVS Persistence ===================== */
static esp_err_t save_rules_to_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Count enabled rules */
    uint8_t count = 0;
    for (uint8_t i = 0; i < CUSTOM_RULE_MAX_SLOTS; i++) {
        if (s_rules[i].enabled || strlen(s_rules[i].name) > 0) {
            count = i + 1;
        }
    }

    ret = nvs_set_u8(handle, NVS_KEY_RULE_COUNT, count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save rule count: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }

    for (uint8_t i = 0; i < count; i++) {
        char key[16];
        snprintf(key, sizeof(key), NVS_KEY_RULE_FMT, i);
        ret = nvs_set_blob(handle, key, &s_rules[i], sizeof(custom_rule_t));
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save rule %u: %s", i, esp_err_to_name(ret));
        }
    }

    ret = nvs_commit(handle);
    nvs_close(handle);

    ESP_LOGI(TAG, "Saved %u rules to NVS", count);
    return ret;
}

static esp_err_t load_rules_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        /* First boot, no rules yet */
        memset(s_rules, 0, sizeof(s_rules));
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    uint8_t count = 0;
    ret = nvs_get_u8(handle, NVS_KEY_RULE_COUNT, &count);
    if (ret != ESP_OK) {
        count = 0;
    }

    if (count > CUSTOM_RULE_MAX_SLOTS) {
        count = CUSTOM_RULE_MAX_SLOTS;
    }

    memset(s_rules, 0, sizeof(s_rules));

    for (uint8_t i = 0; i < count; i++) {
        char key[16];
        snprintf(key, sizeof(key), NVS_KEY_RULE_FMT, i);
        size_t size = sizeof(custom_rule_t);
        ret = nvs_get_blob(handle, key, &s_rules[i], &size);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to load rule %u: %s", i, esp_err_to_name(ret));
        }
    }

    nvs_close(handle);

    uint8_t active = 0;
    for (uint8_t i = 0; i < CUSTOM_RULE_MAX_SLOTS; i++) {
        if (s_rules[i].enabled) active++;
    }
    ESP_LOGI(TAG, "Loaded %u rules from NVS (%u active)", count, active);
    return ESP_OK;
}

/* ===================== Public API ===================== */
esp_err_t custom_rule_app_init(void)
{
    if (s_initialized) {
        return ERR_ALREADY_INITIALIZED;
    }

    /* Load rules from NVS */
    load_rules_from_nvs();

    /* Subscribe to events */
    esp_err_t ret = event_bus_subscribe(EV_APP_MODE_CHANGED, custom_rule_on_mode_changed, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to EV_APP_MODE_CHANGED");
        return ret;
    }

    ret = event_bus_subscribe(EV_TOUCH_A_SINGLE_CLICK, custom_rule_on_touch_event, NULL);
    if (ret != ESP_OK) return ret;
    ret = event_bus_subscribe(EV_TOUCH_B_SINGLE_CLICK, custom_rule_on_touch_event, NULL);
    if (ret != ESP_OK) return ret;
    ret = event_bus_subscribe(EV_TOUCH_C_SINGLE_CLICK, custom_rule_on_touch_event, NULL);
    if (ret != ESP_OK) return ret;
    ret = event_bus_subscribe(EV_TOUCH_D_SINGLE_CLICK, custom_rule_on_touch_event, NULL);
    if (ret != ESP_OK) return ret;
    ret = event_bus_subscribe(EV_TOUCH_A_DOUBLE_CLICK, custom_rule_on_touch_event, NULL);
    if (ret != ESP_OK) return ret;
    ret = event_bus_subscribe(EV_TOUCH_B_DOUBLE_CLICK, custom_rule_on_touch_event, NULL);
    if (ret != ESP_OK) return ret;
    ret = event_bus_subscribe(EV_TOUCH_C_DOUBLE_CLICK, custom_rule_on_touch_event, NULL);
    if (ret != ESP_OK) return ret;
    ret = event_bus_subscribe(EV_TOUCH_D_DOUBLE_CLICK, custom_rule_on_touch_event, NULL);
    if (ret != ESP_OK) return ret;
    ret = event_bus_subscribe(EV_TOUCH_A_LONG_PRESS, custom_rule_on_touch_event, NULL);
    if (ret != ESP_OK) return ret;
    ret = event_bus_subscribe(EV_TOUCH_B_LONG_PRESS, custom_rule_on_touch_event, NULL);
    if (ret != ESP_OK) return ret;
    ret = event_bus_subscribe(EV_TOUCH_C_LONG_PRESS, custom_rule_on_touch_event, NULL);
    if (ret != ESP_OK) return ret;
    ret = event_bus_subscribe(EV_TOUCH_D_LONG_PRESS, custom_rule_on_touch_event, NULL);
    if (ret != ESP_OK) return ret;

    ret = event_bus_subscribe(EV_RADAR_PRESENCE, custom_rule_on_radar_event, NULL);
    if (ret != ESP_OK) return ret;
    ret = event_bus_subscribe(EV_SENSOR_RADAR_CLEAR, custom_rule_on_radar_event, NULL);
    if (ret != ESP_OK) return ret;

    /* Create periodic timer (check every 10 seconds for time-based triggers) */
    const esp_timer_create_args_t timer_args = {
        .callback = rule_timer_callback,
        .arg = NULL,
        .name = "rule_timer",
        .dispatch_method = ESP_TIMER_TASK,
    };
    ret = esp_timer_create(&timer_args, &s_rule_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create rule timer");
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Custom rule app initialized");
    return ESP_OK;
}

esp_err_t custom_rule_app_start(void)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    if (s_running) {
        return ERR_BUSY;
    }

    /* Clear last trigger times */
    memset(s_last_trigger_time, 0, sizeof(s_last_trigger_time));

    s_running = true;

    /* Apply default lighting */
    led_service_set_mode(LED_MODE_AMBIENT);
    led_service_set_color(100, 100, 255);
    led_service_set_effect(LED_EFFECT_STEADY);

    /* LCD */
    lcd_service_info_set_title("Custom Mode");
    lcd_service_page_switch_to(LCD_PAGE_INFO);
    lcd_service_info_update();

    /* Start periodic timer */
    esp_timer_start_periodic(s_rule_timer, 10000000);  /* 10 sec */

    event_bus_publish_simple(EV_APP_STATE_CHANGED);
    ESP_LOGI(TAG, "Custom rule app started (%u active rules)",
             custom_rule_app_get_active_rule_count());
    return ESP_OK;
}

esp_err_t custom_rule_app_stop(void)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    if (!s_running) {
        return ERR_BUSY;
    }

    if (s_rule_timer != NULL) {
        esp_timer_stop(s_rule_timer);
    }

    led_service_turn_off();
    lcd_service_page_switch_to(LCD_PAGE_EXPRESSION);
    lcd_service_expression_set(LCD_EXPRESSION_NORMAL);

    s_running = false;

    event_bus_publish_simple(EV_APP_STATE_CHANGED);
    ESP_LOGI(TAG, "Custom rule app stopped");
    return ESP_OK;
}

esp_err_t custom_rule_app_set_rule(uint8_t slot, const custom_rule_t *rule)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    if (slot >= CUSTOM_RULE_MAX_SLOTS || rule == NULL) {
        return ERR_INVALID_PARAM;
    }

    memcpy(&s_rules[slot], rule, sizeof(custom_rule_t));
    s_last_trigger_time[slot] = 0;

    ESP_LOGI(TAG, "Rule slot %u updated: %s (enabled=%d, actions=%u)",
             slot, rule->name, rule->enabled, rule->action_count);

    return save_rules_to_nvs();
}

esp_err_t custom_rule_app_get_rule(uint8_t slot, custom_rule_t *rule)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    if (slot >= CUSTOM_RULE_MAX_SLOTS || rule == NULL) {
        return ERR_INVALID_PARAM;
    }

    memcpy(rule, &s_rules[slot], sizeof(custom_rule_t));
    return ESP_OK;
}

esp_err_t custom_rule_app_delete_rule(uint8_t slot)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    if (slot >= CUSTOM_RULE_MAX_SLOTS) {
        return ERR_INVALID_PARAM;
    }

    memset(&s_rules[slot], 0, sizeof(custom_rule_t));
    s_last_trigger_time[slot] = 0;

    ESP_LOGI(TAG, "Rule slot %u deleted", slot);
    return save_rules_to_nvs();
}

esp_err_t custom_rule_app_set_rule_enabled(uint8_t slot, bool enable)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    if (slot >= CUSTOM_RULE_MAX_SLOTS) {
        return ERR_INVALID_PARAM;
    }

    s_rules[slot].enabled = enable;
    s_last_trigger_time[slot] = 0;

    ESP_LOGI(TAG, "Rule slot %u %s", slot, enable ? "enabled" : "disabled");
    return save_rules_to_nvs();
}

uint8_t custom_rule_app_get_active_rule_count(void)
{
    uint8_t count = 0;
    for (uint8_t i = 0; i < CUSTOM_RULE_MAX_SLOTS; i++) {
        if (s_rules[i].enabled) count++;
    }
    return count;
}
