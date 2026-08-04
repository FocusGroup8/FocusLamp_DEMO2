/*
 * touch_service.c - Touch service implementation
 */

#include "touch_service.h"
#include "event_bus.h"
#include "event_def.h"
#include "touch_driver.h"
#include "data_type.h"
#include "esp_log.h"

static const char *TAG = "touch_service";

static bool s_initialized = false;
static bool s_running = false;

/* ===================== Touch State Machine ===================== */
/*
 * Touch state machine per point:
 *   IDLE -> PRESSED (on stable press)
 *   PRESSED -> HELD (after long press threshold)
 *   PRESSED/HELD -> RELEASED (on stable release)
 *   During PRESSED, track press duration for long-press detection.
 *   On release, measure interval for double-click detection.
 */
typedef enum {
    TOUCH_STATE_IDLE = 0,
    TOUCH_STATE_PRESSED,
    TOUCH_STATE_HELD,
    TOUCH_STATE_WAITING_DOUBLE_CLICK,
} touch_fsm_state_t;

/* Per-point touch state */
typedef struct {
    touch_fsm_state_t   state;
    bool                last_stable_state;   /* Previous stable GPIO state */
    uint32_t            press_start_time_ms; /* When press started */
    uint32_t            last_release_time_ms;/* When last release occurred */
    uint8_t             tap_count;           /* Consecutive tap counter */
    bool                long_press_sent;     /* Long press already published */
} touch_point_state_t;

static touch_point_state_t s_touch_points[TOUCH_POINT_MAX];
static uint8_t s_active_points = 0;
static uint8_t s_last_active_points = 0;

/* Timing constants (milliseconds) */
#define TOUCH_LONG_PRESS_MS     600
#define TOUCH_DOUBLE_CLICK_MS   300

/* ===================== Helpers ===================== */

static event_type_t touch_get_click_event(touch_point_t point, touch_event_t touch_evt)
{
    /* Map touch_event_t to the appropriate EV_TOUCH_* event type based on point */
    uint16_t base = 0;
    switch (point) {
        case TOUCH_POINT_A: base = 0x0300; break;
        case TOUCH_POINT_B: base = 0x0310; break;
        case TOUCH_POINT_C: base = 0x0320; break;
        case TOUCH_POINT_D: base = 0x0330; break;
        default:            return EV_SYS_ERROR;
    }

    switch (touch_evt) {
        case TOUCH_EVENT_SINGLE_CLICK:  return (event_type_t)(base + 0);
        case TOUCH_EVENT_DOUBLE_CLICK:  return (event_type_t)(base + 1);
        case TOUCH_EVENT_LONG_PRESS:    return (event_type_t)(base + 2);
        case TOUCH_EVENT_SLIDE_UP:      return (event_type_t)(base + 3);
        case TOUCH_EVENT_SLIDE_DOWN:    return (event_type_t)(base + 4);
        case TOUCH_EVENT_SLIDE_LEFT:    return (event_type_t)(base + 5);
        case TOUCH_EVENT_SLIDE_RIGHT:   return (event_type_t)(base + 6);
        case TOUCH_EVENT_RELEASE:       return (event_type_t)(base + 7);
        default:                        return EV_SYS_ERROR;
    }
}

static void touch_publish_event(touch_point_t point, touch_event_t evt)
{
    event_type_t type = touch_get_click_event(point, evt);
    if (type == EV_SYS_ERROR) {
        return;
    }

    uint8_t point_id = (uint8_t)point;
    event_t event = {
        .type = type,
        .data = &point_id,
        .data_size = sizeof(point_id),
        .timestamp = event_bus_get_timestamp(),
    };
    event_bus_publish(&event);
}

/* ===================== Combo Detection ===================== */

static void touch_check_combos(void)
{
    /* Check common two-point combos and publish combo events */
    static const struct {
        touch_point_t a;
        touch_point_t b;
        event_type_t  event;
    } combos[] = {
        {TOUCH_POINT_A, TOUCH_POINT_B, EV_TOUCH_AB_COMBO},
        {TOUCH_POINT_A, TOUCH_POINT_C, EV_TOUCH_AC_COMBO},
        {TOUCH_POINT_B, TOUCH_POINT_C, EV_TOUCH_BC_COMBO},
        {TOUCH_POINT_A, TOUCH_POINT_D, EV_TOUCH_AD_COMBO},
        {TOUCH_POINT_B, TOUCH_POINT_D, EV_TOUCH_BD_COMBO},
        {TOUCH_POINT_C, TOUCH_POINT_D, EV_TOUCH_CD_COMBO},
    };

    for (size_t i = 0; i < sizeof(combos) / sizeof(combos[0]); i++) {
        uint8_t mask_a = (uint8_t)(1 << combos[i].a);
        uint8_t mask_b = (uint8_t)(1 << combos[i].b);
        bool now_active  = (s_active_points & mask_a) && (s_active_points & mask_b);
        bool last_active = (s_last_active_points & mask_a) && (s_last_active_points & mask_b);

        if (now_active && !last_active) {
            uint8_t point_id = (uint8_t)(combos[i].a | (combos[i].b << 4));
            event_t event = {
                .type = combos[i].event,
                .data = &point_id,
                .data_size = sizeof(point_id),
                .timestamp = event_bus_get_timestamp(),
            };
            event_bus_publish(&event);
        }
    }
}

/* ===================== Touch Scanning ===================== */

static void touch_service_scan(void)
{
    uint32_t now = event_bus_get_timestamp();

    /* Update active points bitmask from debounced driver state */
    s_last_active_points = s_active_points;
    s_active_points = touch_driver_get_active_points();

    /* Check combo events before per-point handling */
    touch_check_combos();

    /* Scan all touch points */
    for (int i = 0; i < TOUCH_POINT_MAX; i++) {
        touch_point_t point = (touch_point_t)i;
        touch_point_state_t *ts = &s_touch_points[i];
        bool current = touch_driver_get_state(point);

        switch (ts->state) {
            case TOUCH_STATE_IDLE:
                ts->long_press_sent = false;
                if (current) {
                    /* Press detected */
                    ts->state = TOUCH_STATE_PRESSED;
                    ts->press_start_time_ms = now;
                }
                break;

            case TOUCH_STATE_PRESSED:
                if (!current) {
                    /* Released - check for double-click */
                    uint32_t press_duration = now - ts->press_start_time_ms;
                    if (press_duration >= TOUCH_LONG_PRESS_MS) {
                        /* Already long enough, but released - publish release */
                        touch_publish_event(point, TOUCH_EVENT_RELEASE);
                        ts->state = TOUCH_STATE_IDLE;
                        ts->tap_count = 0;
                    } else {
                        ts->state = TOUCH_STATE_WAITING_DOUBLE_CLICK;
                        ts->last_release_time_ms = now;
                        ts->tap_count++;
                    }
                } else {
                    /* Still pressed - check for long press */
                    uint32_t press_duration = now - ts->press_start_time_ms;
                    if (press_duration >= TOUCH_LONG_PRESS_MS && !ts->long_press_sent) {
                        touch_publish_event(point, TOUCH_EVENT_LONG_PRESS);
                        ts->long_press_sent = true;
                        ts->state = TOUCH_STATE_HELD;
                    }
                }
                break;

            case TOUCH_STATE_HELD:
                if (!current) {
                    /* Released after long press */
                    touch_publish_event(point, TOUCH_EVENT_RELEASE);
                    ts->state = TOUCH_STATE_IDLE;
                    ts->tap_count = 0;
                }
                break;

            case TOUCH_STATE_WAITING_DOUBLE_CLICK:
                /* Waiting for double-click timeout or next press */
                if (current) {
                    /* Another press within timeout - double-click */
                    touch_publish_event(point, TOUCH_EVENT_DOUBLE_CLICK);
                    ts->state = TOUCH_STATE_PRESSED;
                    ts->press_start_time_ms = now;
                    ts->tap_count = 0;
                } else if ((now - ts->last_release_time_ms) > TOUCH_DOUBLE_CLICK_MS) {
                    /* Timeout - single click + release */
                    touch_publish_event(point, TOUCH_EVENT_SINGLE_CLICK);
                    touch_publish_event(point, TOUCH_EVENT_RELEASE);
                    ts->state = TOUCH_STATE_IDLE;
                    ts->tap_count = 0;
                }
                break;

            default:
                break;
        }

        ts->last_stable_state = current;
    }
}

/* ===================== Timer Callback ===================== */

static __attribute__((unused)) void touch_service_timer_cb(void *arg)
{
    (void)arg;
    touch_service_process();
}

/* ===================== Event Callbacks ===================== */

static void touch_service_event_handler(event_t *event, void *context)
{
    (void)context;

    uint16_t type = (uint16_t)event->type;

    /* Only handle A/B/C/D single/double/long-press touch events for diagnostics */
    if (type < 0x0300U || type > 0x0337U) {
        return;
    }

    uint8_t point_idx = (uint8_t)((type - 0x0300U) >> 4);
    uint8_t event_kind = type & 0x0FU;

    if (point_idx >= 4) {
        return;
    }

    (void)event_kind;
}

/* ===================== Public API ===================== */

esp_err_t touch_service_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing touch service...");

    esp_err_t ret = touch_driver_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Touch driver init failed");
        return ret;
    }

    /* Initialize per-point state */
    for (int i = 0; i < TOUCH_POINT_MAX; i++) {
        s_touch_points[i].state = TOUCH_STATE_IDLE;
        s_touch_points[i].last_stable_state = false;
        s_touch_points[i].press_start_time_ms = 0;
        s_touch_points[i].last_release_time_ms = 0;
        s_touch_points[i].tap_count = 0;
        s_touch_points[i].long_press_sent = false;
    }

    s_active_points = 0;
    s_last_active_points = 0;

    /* Subscribe to EV_TOUCH_* diagnostic events (single/double/long press) */
    static const event_type_t s_touch_event_bases[] = {
        EV_TOUCH_A_SINGLE_CLICK,
        EV_TOUCH_B_SINGLE_CLICK,
        EV_TOUCH_C_SINGLE_CLICK,
        EV_TOUCH_D_SINGLE_CLICK,
    };

    for (int i = 0; i < sizeof(s_touch_event_bases) / sizeof(s_touch_event_bases[0]); i++) {
        for (int k = 0; k < 3; k++) {
            ret = event_bus_subscribe((event_type_t)(s_touch_event_bases[i] + k),
                                      touch_service_event_handler, NULL);
            (void)ret;
        }
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Touch service initialized");
    return ESP_OK;
}

esp_err_t touch_service_start(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_running) {
        return ESP_OK;
    }

    s_running = true;
    ESP_LOGI(TAG, "Touch service started");
    return ESP_OK;
}

esp_err_t touch_service_stop(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_running = false;
    ESP_LOGI(TAG, "Touch service stopped");
    return ESP_OK;
}

void touch_service_process(void)
{
    if (!s_running) {
        return;
    }
    touch_driver_scan();
    touch_service_scan();
}

uint8_t touch_service_get_active_points(void)
{
    return s_active_points;
}

bool touch_service_is_combo_active(touch_point_t point_a, touch_point_t point_b)
{
    if (point_a >= TOUCH_POINT_MAX || point_b >= TOUCH_POINT_MAX) {
        return false;
    }
    uint8_t mask_a = (uint8_t)(1 << point_a);
    uint8_t mask_b = (uint8_t)(1 << point_b);
    return (s_active_points & mask_a) && (s_active_points & mask_b);
}