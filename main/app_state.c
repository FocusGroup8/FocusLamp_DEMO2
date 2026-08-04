/*
 * app_state.c - Application state machine implementation for FocusLamp
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "app_state.h"
#include "event_bus.h"
#include "device_state.h"

static const char *TAG = "app_state";

/* ===================== Internal State ===================== */

static app_state_t s_current_state = APP_STATE_INIT;

/* Simple linked-list of state change handlers */
typedef struct state_handler_node {
    app_state_t              state;     /* Monitored state (or -1 for all) */
    state_change_handler_t   handler;
    void                    *context;
    struct state_handler_node *next;
} state_handler_node_t;

static state_handler_node_t *s_handler_list = NULL;

/* ===================== State Name Lookup ===================== */

static const char *app_state_to_string(app_state_t state)
{
    switch (state) {
        case APP_STATE_INIT:        return "APP_STATE_INIT";
        case APP_STATE_IDLE:        return "APP_STATE_IDLE";
        case APP_STATE_LIGHTING:    return "APP_STATE_LIGHTING";
        case APP_STATE_FOCUS:       return "APP_STATE_FOCUS";
        case APP_STATE_COMPANION:   return "APP_STATE_COMPANION";
        case APP_STATE_ARM_ACTION:  return "APP_STATE_ARM_ACTION";
        case APP_STATE_MUSIC_RHYTHM: return "APP_STATE_MUSIC_RHYTHM";
        case APP_STATE_GAME:        return "APP_STATE_GAME";
        case APP_STATE_VOICE:           return "APP_STATE_VOICE";
        case APP_STATE_EXERCISE_FOLLOW: return "APP_STATE_EXERCISE_FOLLOW";
        case APP_STATE_CUSTOM_RULE:     return "APP_STATE_CUSTOM_RULE";
        case APP_STATE_SLEEP:           return "APP_STATE_SLEEP";
        case APP_STATE_ERROR:       return "APP_STATE_ERROR";
        default:                    return "UNKNOWN";
    }
}

/* ===================== Event Publication ===================== */

static void publish_mode_changed_event(app_state_t old_state, app_state_t new_state)
{
    /* Pack old and new state into a simple data structure for the event */
    uint8_t state_data[2] = { (uint8_t)old_state, (uint8_t)new_state };

    event_t ev = {
        .type       = EV_APP_MODE_CHANGED,
        .data       = state_data,
        .data_size  = sizeof(state_data),
        .timestamp  = event_bus_get_timestamp(),
    };

    event_bus_publish(&ev);
}

/* ===================== Handler Notification ===================== */

static void notify_handlers(app_state_t old_state, app_state_t new_state)
{
    state_handler_node_t *node = s_handler_list;
    while (node != NULL) {
        if (node->state == new_state || (int)node->state == -1) {
            node->handler(old_state, new_state, node->context);
        }
        node = node->next;
    }
}

/* ===================== Public API ===================== */

esp_err_t app_state_manager_init(void)
{
    s_current_state = APP_STATE_INIT;
    s_handler_list = NULL;

    device_state_init();
    device_state_set_app_state(s_current_state);

    ESP_LOGI(TAG, "State manager initialized, state = %s", app_state_to_string(s_current_state));

    /* Publish initial state */
    publish_mode_changed_event(APP_STATE_INIT, APP_STATE_INIT);

    return ESP_OK;
}

esp_err_t app_state_manager_set_state(app_state_t state)
{
    if (state > APP_STATE_ERROR) {
        ESP_LOGE(TAG, "Invalid state: %d", state);
        return ESP_ERR_INVALID_ARG;
    }

    if (s_current_state == state) {
        ESP_LOGW(TAG, "State already %s, ignoring", app_state_to_string(state));
        return ESP_OK;
    }

    app_state_t old_state = s_current_state;
    s_current_state = state;

    device_state_set_app_state(state);

    ESP_LOGI(TAG, "State transition: %s -> %s",
             app_state_to_string(old_state),
             app_state_to_string(state));

    /* Notify registered handlers */
    notify_handlers(old_state, state);

    /* Publish mode changed event */
    publish_mode_changed_event(old_state, state);

    return ESP_OK;
}

app_state_t app_state_manager_get_state(void)
{
    return s_current_state;
}

esp_err_t app_state_manager_register_handler(app_state_t state, state_change_handler_t handler, void *context)
{
    if (handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    state_handler_node_t *node = (state_handler_node_t *)malloc(sizeof(state_handler_node_t));
    if (node == NULL) {
        return ESP_ERR_NO_MEM;
    }

    node->state   = state;
    node->handler = handler;
    node->context = context;
    node->next    = s_handler_list;
    s_handler_list = node;

    ESP_LOGD(TAG, "Handler registered for state %s", app_state_to_string(state));

    return ESP_OK;
}