/*
 * arm_action_app.c - Mechanical arm action application implementation
 */

#include "arm_action_app.h"
#include "event_bus.h"
#include "event_def.h"
#include "arm_service.h"
#include "data_type.h"
#include "device_state.h"
#include "error_code.h"
#include "esp_log.h"

static const char *TAG = "arm_action_app";

/* ===================== Private Data ===================== */
static bool s_initialized = false;
static bool s_running = false;
static bool s_learning = false;

/* ===================== Predefined Action Sequences ===================== */

/* Wave: LX-1 (servo 1) ±150 from home(500), 3 loops */
static const action_step_t s_wave_steps[] = {
    { 1, 650, 300, 100 },
    { 1, 350, 300, 100 },
};
static const action_sequence_t s_wave_seq = {
    .steps = s_wave_steps, .step_count = 2, .loop_count = 3,
};

/* Nod: EM3 (servo 0) ±200 from home(1500), 3 loops */
/* Note: Should be LX-5, but servo_service only supports ID 0 and 1 */
static const action_step_t s_nod_steps[] = {
    { 0, 1700, 400, 200 },
    { 0, 1300, 400, 200 },
};
static const action_sequence_t s_nod_seq = {
    .steps = s_nod_steps, .step_count = 2, .loop_count = 3,
};

/* Shake: EM3 (servo 0) ±350 from home(1500), 3 loops */
static const action_step_t s_shake_steps[] = {
    { 0, 1850, 300, 100 },
    { 0, 1150, 300, 100 },
};
static const action_sequence_t s_shake_seq = {
    .steps = s_shake_steps, .step_count = 2, .loop_count = 3,
};

/* Point: extend servo 0 forward */
static const action_step_t s_point_steps[] = {
    { 0, 800, 500, 0 },
    { 1, 700, 500, 0 },
    { 2, 512, 500, 1000 },
};
static const action_sequence_t s_point_seq = {
    .steps = s_point_steps, .step_count = 3, .loop_count = 1,
};

/* Hug: both arms move inward */
static const action_step_t s_hug_steps[] = {
    { 0, 200, 600, 0 },
    { 1, 300, 600, 0 },
    { 2, 400, 600, 2000 },
    { 0, 512, 600, 0 },
    { 1, 512, 600, 0 },
};
static const action_sequence_t s_hug_seq = {
    .steps = s_hug_steps, .step_count = 5, .loop_count = 1,
};

/* Lookup table for predefined actions */
static const action_sequence_t *s_predefined_actions[ARM_ACTION_MAX] = {
    [ARM_ACTION_WAVE] = &s_wave_seq,
    [ARM_ACTION_NOD]  = &s_nod_seq,
    [ARM_ACTION_SHAKE] = &s_shake_seq,
    [ARM_ACTION_POINT] = &s_point_seq,
    [ARM_ACTION_HUG]  = &s_hug_seq,
};

/* ===================== Learn Mode Buffer ===================== */
/* TODO: Implement dynamic storage for learned sequences */
#define LEARN_MAX_STEPS 64
static action_step_t s_learn_buffer[LEARN_MAX_STEPS] __attribute__((unused));
static uint16_t s_learn_count = 0;

/* ===================== Event Handlers ===================== */
static void arm_action_app_on_mode_changed(event_t *event, void *context)
{
    (void)context;
    if (event == NULL || event->data == NULL || event->data_size < sizeof(uint8_t) * 2) {
        return;
    }
    const uint8_t *state_data = (const uint8_t *)event->data;
    app_state_t new_state = (app_state_t)state_data[1];
    if (new_state != APP_STATE_ARM_ACTION && s_running) {
        arm_action_app_stop();
    }
}

/* ===================== Public API ===================== */
esp_err_t arm_action_app_init(void)
{
    if (s_initialized) {
        return ERR_ALREADY_INITIALIZED;
    }

    esp_err_t ret = event_bus_subscribe(EV_APP_MODE_CHANGED, arm_action_app_on_mode_changed, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to EV_APP_MODE_CHANGED");
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Arm action app initialized");
    return ESP_OK;
}

esp_err_t arm_action_app_play(arm_action_id_t action_id)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    if (action_id >= ARM_ACTION_MAX) {
        return ERR_INVALID_PARAM;
    }

    const action_sequence_t *seq = s_predefined_actions[action_id];
    if (seq == NULL) {
        return ERR_NOT_FOUND;
    }

    esp_err_t ret = arm_service_load_action(seq);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = arm_service_start_action();
    if (ret == ESP_OK) {
        s_running = true;
        ESP_LOGI(TAG, "Playing action: %d", action_id);
    }
    return ret;
}

esp_err_t arm_action_app_play_custom(const action_sequence_t *seq)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    if (seq == NULL || seq->steps == NULL || seq->step_count == 0) {
        return ERR_INVALID_PARAM;
    }

    esp_err_t ret = arm_service_load_action(seq);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = arm_service_start_action();
    if (ret == ESP_OK) {
        s_running = true;
        ESP_LOGI(TAG, "Playing custom action");
    }
    return ret;
}

esp_err_t arm_action_app_stop(void)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }

    esp_err_t ret = arm_service_stop_action();
    s_running = false;
    ESP_LOGI(TAG, "Arm action stopped");
    return ret;
}

esp_err_t arm_action_app_learn_start(void)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    if (s_learning) {
        return ERR_BUSY;
    }

    s_learn_count = 0;
    s_learning = true;
    ESP_LOGI(TAG, "Learn mode started");
    return ESP_OK;
}

esp_err_t arm_action_app_learn_stop(void)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    if (!s_learning) {
        return ERR_BUSY;
    }

    s_learning = false;
    ESP_LOGI(TAG, "Learn mode stopped, recorded %d steps", s_learn_count);
    return ESP_OK;
}

esp_err_t arm_action_app_save(const char *name)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    if (name == NULL) {
        return ERR_INVALID_PARAM;
    }

    /* TODO: Persist learned sequence to NVS or filesystem */
    ESP_LOGI(TAG, "Action saved: %s (%d steps)", name, s_learn_count);
    return ESP_OK;
}

esp_err_t arm_action_app_load(const char *name)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    if (name == NULL) {
        return ERR_INVALID_PARAM;
    }

    /* TODO: Load sequence from NVS or filesystem, then execute */
    ESP_LOGI(TAG, "Action loaded: %s", name);
    return ESP_OK;
}