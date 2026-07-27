/*
 * game_app.c - Mini-game application implementation
 */

#include "game_app.h"
#include "event_bus.h"
#include "event_def.h"
#include "led_service.h"
#include "lcd_service.h"
#include "audio_service.h"
#include "servo_service.h"
#include "arm_service.h"
#include "touch_service.h"
#include "data_type.h"
#include "device_state.h"
#include "error_code.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_random.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "game_app";

/* ===================== Timing / Configuration ===================== */
#define RPS_CYCLE_PERIOD_MS      500
#define RHYTHM_BEAT_PERIOD_MS    500
#define RHYTHM_WINDOW_MS         200
#define REACT_DELAY_MIN_MS       1000
#define REACT_DELAY_MAX_MS       3000
#define MEMORY_FLASH_PERIOD_MS   600

#define TONE_LOW_FREQ_HZ         220
#define TONE_LOW_DURATION_MS     300

/* ===================== Color Helpers ===================== */
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} game_color_t;

static const game_color_t s_colors[] = {
    [0] = {255, 0,   0},   /* Red    - Rock / Touch A */
    [1] = {0,   255, 0},   /* Green  - Paper / Touch B */
    [2] = {0,   0,   255}, /* Blue   - Scissors / Touch C */
    [3] = {255, 255, 0},   /* Yellow - Touch D */
};

static const char *s_color_names[] = {
    [0] = "Rock",
    [1] = "Paper",
    [2] = "Scissors",
    [3] = "Yellow",
};

/* ===================== Private Data ===================== */
static bool s_initialized = false;
static bool s_running = false;
static game_id_t s_current_game = GAME_ID_MAX;
static esp_timer_handle_t s_game_timer = NULL;

/* ===================== Game-Specific State ===================== */
typedef enum {
    RPS_STATE_CYCLING = 0,
    RPS_STATE_RESULT,
} rps_state_t;

static rps_state_t s_rps_state;
static uint8_t     s_rps_choice;

typedef enum {
    REACT_STATE_WAITING = 0,
    REACT_STATE_READY,
    REACT_STATE_DONE,
} react_state_t;

static react_state_t s_react_state;
static uint32_t      s_react_start_ms;

static uint32_t s_rhythm_beat_ms;
static uint32_t s_rhythm_score;

typedef enum {
    MEMORY_STATE_SHOWING = 0,
    MEMORY_STATE_INPUT,
    MEMORY_STATE_RESULT,
} memory_state_t;

static memory_state_t s_memory_state;
static uint8_t        s_memory_sequence[4];
static uint8_t        s_memory_input_index;
static uint8_t        s_memory_show_index;

/* ===================== Forward Declarations ===================== */
static void game_timer_callback(void *arg);
static void rps_timer_callback(void);
static void rhythm_timer_callback(void);
static void memory_timer_callback(void);
static void react_timer_callback(void);

static void rps_handle_touch(void);
static void react_handle_touch(void);
static void rhythm_handle_touch(void);
static void memory_handle_touch(event_type_t type);

/* ===================== Timer Helpers ===================== */
static void game_timer_start_periodic(uint32_t period_ms)
{
    if (s_game_timer == NULL) {
        return;
    }
    esp_timer_stop(s_game_timer);
    esp_timer_start_periodic(s_game_timer, (uint64_t)period_ms * 1000ULL);
}

static void game_timer_start_once(uint32_t delay_ms)
{
    if (s_game_timer == NULL) {
        return;
    }
    esp_timer_stop(s_game_timer);
    esp_timer_start_once(s_game_timer, (uint64_t)delay_ms * 1000ULL);
}

static void game_timer_stop(void)
{
    if (s_game_timer != NULL) {
        esp_timer_stop(s_game_timer);
    }
}

/* ===================== LCD Helpers ===================== */
static void game_lcd_show(const char *title)
{
    lcd_service_page_switch_to(LCD_PAGE_INFO);
    lcd_service_info_set_title(title);
    lcd_service_info_update();
}

/* ===================== Event Handlers ===================== */
static void game_app_on_mode_changed(event_t *event, void *context)
{
    if (event == NULL || event->data == NULL || event->data_size < sizeof(uint8_t) * 2) {
        return;
    }

    uint8_t *state_data = (uint8_t *)event->data;
    app_state_t new_state = (app_state_t)state_data[1];
    if (new_state != APP_STATE_GAME && s_running) {
        game_app_stop();
    }
}

static void game_app_on_touch_event(event_t *event, void *context)
{
    if (event == NULL || !s_running) {
        return;
    }

    switch (s_current_game) {
        case GAME_ID_ROCK_PAPER_SCISSORS:
            if (event->type == EV_TOUCH_A_SINGLE_CLICK) {
                rps_handle_touch();
            }
            break;

        case GAME_ID_REACTION_TEST:
            if (event->type == EV_TOUCH_A_SINGLE_CLICK) {
                react_handle_touch();
            }
            break;

        case GAME_ID_RHYTHM_GAME:
            if (event->type == EV_TOUCH_A_SINGLE_CLICK) {
                rhythm_handle_touch();
            }
            break;

        case GAME_ID_MEMORY_GAME:
            if (event->type == EV_TOUCH_A_SINGLE_CLICK ||
                event->type == EV_TOUCH_B_SINGLE_CLICK ||
                event->type == EV_TOUCH_C_SINGLE_CLICK ||
                event->type == EV_TOUCH_D_SINGLE_CLICK) {
                memory_handle_touch(event->type);
            }
            break;

        default:
            break;
    }
}

/* ===================== Shared Timer Dispatcher ===================== */
static void game_timer_callback(void *arg)
{
    if (!s_running) {
        return;
    }

    switch (s_current_game) {
        case GAME_ID_ROCK_PAPER_SCISSORS:
            rps_timer_callback();
            break;
        case GAME_ID_REACTION_TEST:
            react_timer_callback();
            break;
        case GAME_ID_RHYTHM_GAME:
            rhythm_timer_callback();
            break;
        case GAME_ID_MEMORY_GAME:
            memory_timer_callback();
            break;
        default:
            break;
    }
}

/* ===================== Rock-Paper-Scissors ===================== */
static esp_err_t game_start_rock_paper_scissors(void)
{
    s_rps_state = RPS_STATE_CYCLING;
    s_rps_choice = 0;

    game_lcd_show("RPS");
    led_service_set_color(s_colors[0].r, s_colors[0].g, s_colors[0].b);
    game_timer_start_periodic(RPS_CYCLE_PERIOD_MS);

    ESP_LOGI(TAG, "Rock-Paper-Scissors started");
    return ESP_OK;
}

static void rps_timer_callback(void)
{
    if (s_rps_state != RPS_STATE_CYCLING) {
        return;
    }

    s_rps_choice = (s_rps_choice + 1) % 3;
    led_service_set_color(s_colors[s_rps_choice].r,
                          s_colors[s_rps_choice].g,
                          s_colors[s_rps_choice].b);
}

static void rps_handle_touch(void)
{
    if (s_rps_state != RPS_STATE_CYCLING) {
        return;
    }

    game_timer_stop();
    s_rps_state = RPS_STATE_RESULT;

    uint8_t device_choice = (uint8_t)(esp_random() % 3);
    led_service_set_color(s_colors[device_choice].r,
                          s_colors[device_choice].g,
                          s_colors[device_choice].b);

    char title[32];
    snprintf(title, sizeof(title), "%s", s_color_names[device_choice]);
    game_lcd_show(title);

    audio_service_play_alert();
    ESP_LOGI(TAG, "RPS result: %s", s_color_names[device_choice]);
}

/* ===================== Reaction Test ===================== */
static esp_err_t game_start_reaction_test(void)
{
    s_react_state = REACT_STATE_WAITING;

    game_lcd_show("React");
    led_service_turn_off();

    uint32_t delay_ms = REACT_DELAY_MIN_MS +
                        (uint32_t)(esp_random() % (REACT_DELAY_MAX_MS - REACT_DELAY_MIN_MS + 1));
    game_timer_start_once(delay_ms);

    ESP_LOGI(TAG, "Reaction Test started, delay=%u ms", delay_ms);
    return ESP_OK;
}

static void react_timer_callback(void)
{
    if (s_react_state != REACT_STATE_WAITING) {
        return;
    }

    led_service_set_color(0, 255, 0);
    s_react_start_ms = event_bus_get_timestamp();
    s_react_state = REACT_STATE_READY;
    game_lcd_show("Go!");
}

static void react_handle_touch(void)
{
    char title[32];

    switch (s_react_state) {
        case REACT_STATE_WAITING:
            game_timer_stop();
            audio_service_play_tone(TONE_LOW_FREQ_HZ, TONE_LOW_DURATION_MS);
            game_lcd_show("Early!");
            s_react_state = REACT_STATE_DONE;
            break;

        case REACT_STATE_READY: {
            uint32_t now = event_bus_get_timestamp();
            uint32_t reaction_ms = now - s_react_start_ms;
            audio_service_play_alert();
            snprintf(title, sizeof(title), "%lu ms", (unsigned long)reaction_ms);
            game_lcd_show(title);
            s_react_state = REACT_STATE_DONE;
            break;
        }

        case REACT_STATE_DONE:
        default:
            break;
    }
}

/* ===================== Rhythm Game ===================== */
static esp_err_t game_start_rhythm_game(void)
{
    s_rhythm_score = 0;
    s_rhythm_beat_ms = 0;

    game_lcd_show("Rhythm");
    led_service_set_color(0, 0, 255);
    game_timer_start_periodic(RHYTHM_BEAT_PERIOD_MS);

    ESP_LOGI(TAG, "Rhythm Game started");
    return ESP_OK;
}

static void rhythm_timer_callback(void)
{
    s_rhythm_beat_ms = event_bus_get_timestamp();
    led_service_set_color(0, 0, 255);
}

static void rhythm_handle_touch(void)
{
    uint32_t now = event_bus_get_timestamp();
    uint32_t delta = (now > s_rhythm_beat_ms) ? (now - s_rhythm_beat_ms) : 0;

    char title[32];
    if (delta <= RHYTHM_WINDOW_MS) {
        s_rhythm_score++;
        led_service_set_color(0, 255, 0);
        snprintf(title, sizeof(title), "Score %lu", (unsigned long)s_rhythm_score);
        audio_service_play_alert();
    } else {
        led_service_set_color(255, 0, 0);
        snprintf(title, sizeof(title), "Miss");
        audio_service_play_tone(TONE_LOW_FREQ_HZ, TONE_LOW_DURATION_MS);
    }
    game_lcd_show(title);
}

/* ===================== Memory Game ===================== */
static esp_err_t game_start_memory_game(void)
{
    for (int i = 0; i < 4; i++) {
        s_memory_sequence[i] = (uint8_t)(esp_random() % 4);
    }
    s_memory_state = MEMORY_STATE_SHOWING;
    s_memory_input_index = 0;
    s_memory_show_index = 0;

    game_lcd_show("Memory");
    game_timer_start_periodic(MEMORY_FLASH_PERIOD_MS);

    ESP_LOGI(TAG, "Memory Game started");
    return ESP_OK;
}

static void memory_timer_callback(void)
{
    if (s_memory_state != MEMORY_STATE_SHOWING) {
        return;
    }

    if (s_memory_show_index >= 8) {
        game_timer_stop();
        s_memory_state = MEMORY_STATE_INPUT;
        led_service_turn_off();
        game_lcd_show("Repeat");
        return;
    }

    if ((s_memory_show_index & 0x01) == 0) {
        uint8_t color_idx = s_memory_sequence[s_memory_show_index / 2];
        led_service_set_color(s_colors[color_idx].r,
                              s_colors[color_idx].g,
                              s_colors[color_idx].b);
    } else {
        led_service_turn_off();
    }
    s_memory_show_index++;
}

static void memory_handle_touch(event_type_t type)
{
    if (s_memory_state != MEMORY_STATE_INPUT) {
        return;
    }

    uint8_t color_idx = 0xFF;
    switch (type) {
        case EV_TOUCH_A_SINGLE_CLICK: color_idx = 0; break;
        case EV_TOUCH_B_SINGLE_CLICK: color_idx = 1; break;
        case EV_TOUCH_C_SINGLE_CLICK: color_idx = 2; break;
        case EV_TOUCH_D_SINGLE_CLICK: color_idx = 3; break;
        default: return;
    }

    led_service_set_color(s_colors[color_idx].r,
                          s_colors[color_idx].g,
                          s_colors[color_idx].b);

    if (s_memory_sequence[s_memory_input_index] != color_idx) {
        audio_service_play_tone(TONE_LOW_FREQ_HZ, TONE_LOW_DURATION_MS);
        game_lcd_show("Wrong");
        s_memory_state = MEMORY_STATE_RESULT;
        game_app_stop();
        return;
    }

    s_memory_input_index++;
    if (s_memory_input_index >= 4) {
        audio_service_play_alert();
        game_lcd_show("Win");
        s_memory_state = MEMORY_STATE_RESULT;
        game_app_stop();
    }
}

/* Lookup table for game starters */
static esp_err_t (*s_game_starters[GAME_ID_MAX])(void) = {
    [GAME_ID_ROCK_PAPER_SCISSORS] = game_start_rock_paper_scissors,
    [GAME_ID_REACTION_TEST]       = game_start_reaction_test,
    [GAME_ID_RHYTHM_GAME]         = game_start_rhythm_game,
    [GAME_ID_MEMORY_GAME]         = game_start_memory_game,
};

/* ===================== Public API ===================== */
esp_err_t game_app_init(void)
{
    if (s_initialized) {
        return ERR_ALREADY_INITIALIZED;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = &game_timer_callback,
        .name     = "game_app_timer"
    };
    esp_err_t ret = esp_timer_create(&timer_args, &s_game_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create game timer");
        return ret;
    }

    ret = event_bus_subscribe(EV_APP_MODE_CHANGED, game_app_on_mode_changed, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to EV_APP_MODE_CHANGED");
        esp_timer_delete(s_game_timer);
        s_game_timer = NULL;
        return ret;
    }

    ret = event_bus_subscribe(EV_TOUCH_A_SINGLE_CLICK, game_app_on_touch_event, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to EV_TOUCH_A_SINGLE_CLICK");
        goto fail;
    }
    ret = event_bus_subscribe(EV_TOUCH_B_SINGLE_CLICK, game_app_on_touch_event, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to EV_TOUCH_B_SINGLE_CLICK");
        goto fail;
    }
    ret = event_bus_subscribe(EV_TOUCH_C_SINGLE_CLICK, game_app_on_touch_event, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to EV_TOUCH_C_SINGLE_CLICK");
        goto fail;
    }
    ret = event_bus_subscribe(EV_TOUCH_D_SINGLE_CLICK, game_app_on_touch_event, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to EV_TOUCH_D_SINGLE_CLICK");
        goto fail;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Game app initialized");
    return ESP_OK;

fail:
    game_app_stop();
    return ret;
}

esp_err_t game_app_start(game_id_t game_id)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    if (s_running) {
        return ERR_BUSY;
    }
    if (game_id >= GAME_ID_MAX) {
        return ERR_INVALID_PARAM;
    }

    s_current_game = game_id;
    esp_err_t ret = s_game_starters[game_id]();
    if (ret != ESP_OK) {
        s_current_game = GAME_ID_MAX;
        return ret;
    }

    s_running = true;

    event_bus_publish_simple(EV_APP_GAME_EVENT);
    ESP_LOGI(TAG, "Game started: %d", game_id);
    return ESP_OK;
}

esp_err_t game_app_stop(void)
{
    if (!s_initialized) {
        return ERR_NOT_INITIALIZED;
    }
    if (!s_running) {
        return ERR_BUSY;
    }

    game_timer_stop();
    led_service_turn_off();
    lcd_service_page_switch_to(LCD_PAGE_EXPRESSION);

    s_running = false;
    s_current_game = GAME_ID_MAX;

    event_bus_publish_simple(EV_APP_STATE_CHANGED);
    ESP_LOGI(TAG, "Game stopped");
    return ESP_OK;
}
