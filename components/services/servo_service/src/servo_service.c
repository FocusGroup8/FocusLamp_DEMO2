/*
 * servo_service.c - Servo control service implementation
 *
 * Contains:
 *   - Basic servo position/speed control (via event bus)
 *   - Recording / Playback of servo motion sequences
 */

#include "servo_service.h"
#include "event_bus.h"
#include "event_def.h"
#include "servo_driver.h"
#include "device_state.h"
#include "esp_log.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "servo_service";

static bool s_initialized = false;
static bool s_enabled = false;

/* Current positions cache (0xFFFF = unknown) */
static uint16_t s_current_positions[SERVO_COUNT];

#if (SERVO_SERVICE_ENABLE == 1)

/* LX bus IDs: 1, 2, 3, 5 */
static uint8_t s_lx_ids[SERVO_SERVICE_LX_COUNT] = {1, 2, 3, 5};

/* Position limits */
static servo_service_limits_t s_limits = {
    .em3_min = SERVO_SERVICE_EM3_MIN_POS,
    .em3_max = SERVO_SERVICE_EM3_MAX_POS,
    .lx_min  = SERVO_SERVICE_LX_MIN_POS,
    .lx_max  = SERVO_SERVICE_LX_MAX_POS,
};

/* Home positions */
static const servo_service_positions_t s_home_positions = {
    .em3_pos = SERVO_SERVICE_HOME_EM3_POS,
    .lx_pos  = {SERVO_SERVICE_HOME_LX_POS_1, SERVO_SERVICE_HOME_LX_POS_2,
                SERVO_SERVICE_HOME_LX_POS_3, SERVO_SERVICE_HOME_LX_POS_5},
};

/* ==================== Recording/Playback State ==================== */

static servo_service_state_t s_current_state = SERVO_SERVICE_STATE_IDLE;
static int s_current_slot = 0;
static int s_frame_counts[SERVO_SERVICE_MAX_SLOTS] = {0};

static uint32_t s_recording_start_time = 0;
static uint32_t s_last_sample_time = 0;
static bool s_have_last_good_frame = false;
static servo_service_frame_t s_last_good_frame = {
    .lx_pos  = {700, 200, 0, 350},
    .em3_pos = 900,
    .time_ms = 0,
};

static TaskHandle_t  s_playback_task_handle = NULL;
static volatile bool s_playback_stop_requested = false;

/* Frame storage in external RAM if available */
static servo_service_frame_t s_recorded_frames[SERVO_SERVICE_MAX_SLOTS]
                                               [SERVO_SERVICE_MAX_FRAMES];

static servo_service_params_t s_params = {
    .sampling_ms    = SERVO_SERVICE_SAMPLING_MS,
    .playback_speed = SERVO_SERVICE_PLAYBACK_SPEED,
    .max_frames     = SERVO_SERVICE_MAX_FRAMES,
    .max_slots      = SERVO_SERVICE_MAX_SLOTS,
};

#define SERVO_SERVICE_INVALID_POS (-32768)
#define SERVO_SERVICE_PLAYBACK_PREPARE_MS 800u
#define SERVO_SERVICE_READ_RETRIES 2

/* ==================== Position Validation ==================== */

static bool em3_pos_valid(int16_t pos)
{
    return pos >= 0 && pos <= 3000;
}

static bool lx_pos_valid(int16_t pos)
{
    return pos >= 0 && pos <= 1000;
}

static bool frame_positions_valid(const servo_service_frame_t *frame)
{
    if (frame == NULL || !em3_pos_valid(frame->em3_pos)) {
        return false;
    }
    for (int i = 0; i < SERVO_SERVICE_LX_COUNT; i++) {
        if (!lx_pos_valid(frame->lx_pos[i])) {
            return false;
        }
    }
    return true;
}

/* ==================== Speed Calculation ==================== */

static uint16_t clamp_em3_speed(uint32_t speed)
{
    if (speed < SERVO_SERVICE_EM3_MIN_SPEED) return SERVO_SERVICE_EM3_MIN_SPEED;
    if (speed > SERVO_SERVICE_EM3_MAX_SPEED) return SERVO_SERVICE_EM3_MAX_SPEED;
    return (uint16_t)speed;
}

static uint16_t em3_speed_for_segment(int16_t from_pos, int16_t to_pos, uint32_t time_ms)
{
    uint32_t diff = (uint32_t)abs(to_pos - from_pos);
    if (time_ms == 0 || diff == 0) {
        return SERVO_SERVICE_EM3_DEFAULT_SPEED;
    }
    uint32_t speed = (diff * 1000u + time_ms - 1u) / time_ms;
    return clamp_em3_speed(speed);
}

/* ==================== Reading with Retry ==================== */

static int16_t read_valid_em3_pos(uint8_t id)
{
    for (int retry = 0; retry < SERVO_SERVICE_READ_RETRIES; retry++) {
        int16_t pos = servo_em3_read_pos(id);
        if (em3_pos_valid(pos)) {
            return pos;
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    return SERVO_SERVICE_INVALID_POS;
}

static int16_t read_valid_lx_pos(uint8_t id)
{
    for (int retry = 0; retry < SERVO_SERVICE_READ_RETRIES; retry++) {
        int16_t pos = servo_lx_read_pos(id);
        if (lx_pos_valid(pos)) {
            return pos;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return SERVO_SERVICE_INVALID_POS;
}

static bool read_current_frame(servo_service_frame_t *frame, uint32_t time_ms,
                               const servo_service_frame_t *fallback)
{
    if (frame == NULL) return false;

    bool valid = true;
    memset(frame, 0, sizeof(*frame));
    frame->time_ms = time_ms;

    int16_t em3_pos = read_valid_em3_pos(SERVO_SERVICE_EM3_ID);
    if (!em3_pos_valid(em3_pos)) {
        if (fallback && em3_pos_valid(fallback->em3_pos)) {
            em3_pos = fallback->em3_pos;
        } else {
            valid = false;
        }
    }
    frame->em3_pos = em3_pos;

    for (int i = 0; i < SERVO_SERVICE_LX_COUNT; i++) {
        int16_t lx_pos = read_valid_lx_pos(s_lx_ids[i]);
        if (!lx_pos_valid(lx_pos)) {
            if (fallback && lx_pos_valid(fallback->lx_pos[i])) {
                lx_pos = fallback->lx_pos[i];
            } else {
                valid = false;
            }
        }
        frame->lx_pos[i] = lx_pos;
    }

    if (valid && frame_positions_valid(frame)) {
        s_last_good_frame = *frame;
        s_have_last_good_frame = true;
        return true;
    }
    return false;
}

static bool sanitize_frame(servo_service_frame_t *frame,
                           const servo_service_frame_t *fallback)
{
    if (frame == NULL) return false;

    if (!em3_pos_valid(frame->em3_pos)) {
        if (fallback && em3_pos_valid(fallback->em3_pos)) {
            frame->em3_pos = fallback->em3_pos;
        } else {
            return false;
        }
    }
    for (int i = 0; i < SERVO_SERVICE_LX_COUNT; i++) {
        if (!lx_pos_valid(frame->lx_pos[i])) {
            if (fallback && lx_pos_valid(fallback->lx_pos[i])) {
                frame->lx_pos[i] = fallback->lx_pos[i];
            } else {
                return false;
            }
        }
    }
    return true;
}

static int sanitize_slot_frames(int slot)
{
    if (slot < 0 || slot >= SERVO_SERVICE_MAX_SLOTS) return 0;

    int count = s_frame_counts[slot];
    if (count <= 0 || count > SERVO_SERVICE_MAX_FRAMES) {
        s_frame_counts[slot] = 0;
        return 0;
    }

    int out = 0;
    servo_service_frame_t prev = {0};
    bool have_prev = false;

    for (int i = 0; i < count; i++) {
        servo_service_frame_t frame = s_recorded_frames[slot][i];
        if (!sanitize_frame(&frame, have_prev ? &prev : NULL)) {
            ESP_LOGW(TAG, "Dropping invalid frame %d in slot %d", i, slot);
            continue;
        }
        if (out == 0) {
            frame.time_ms = 0;
        } else if (frame.time_ms <= prev.time_ms) {
            frame.time_ms = prev.time_ms + s_params.sampling_ms;
        }
        s_recorded_frames[slot][out++] = frame;
        prev = frame;
        have_prev = true;
    }

    s_frame_counts[slot] = out;
    return out;
}

/* ==================== Playback Delay Helper ==================== */

static bool playback_delay(uint32_t ms)
{
    uint32_t elapsed = 0;
    while (elapsed < ms) {
        if (s_playback_stop_requested || s_current_state != SERVO_SERVICE_STATE_PLAYING) {
            return false;
        }
        uint32_t chunk = (ms - elapsed > 20u) ? 20u : (ms - elapsed);
        vTaskDelay(pdMS_TO_TICKS(chunk));
        elapsed += chunk;
    }
    return true;
}

/* ==================== Event Publishing ==================== */

static void publish_state_event(servo_service_state_t old_state,
                                servo_service_state_t new_state)
{
    uint8_t data[4] = { 0, (uint8_t)old_state, 0, (uint8_t)new_state };

    event_t ev = {
        .type = EV_SERVO_STATE_CHANGED,
        .data = data,
        .data_size = sizeof(data),
        .timestamp = event_bus_get_timestamp(),
    };
    event_bus_publish(&ev);
}

/* ==================== Playback Task ==================== */

static void servo_playback_task(void *arg)
{
    int slot = (int)(intptr_t)arg;
    int frame_count = s_frame_counts[slot];
    bool stopped = false;

    /* Enable torque */
    servo_em3_enable_torque(SERVO_SERVICE_EM3_ID, 1);

    /* Move to first frame */
    servo_service_frame_t *start_f = &s_recorded_frames[slot][0];
    servo_em3_move(SERVO_SERVICE_EM3_ID, start_f->em3_pos, SERVO_SERVICE_EM3_DEFAULT_SPEED);
    servo_lx_move_group(s_lx_ids, start_f->lx_pos, SERVO_SERVICE_LX_COUNT,
                        SERVO_SERVICE_PLAYBACK_PREPARE_MS);
    if (!playback_delay(SERVO_SERVICE_PLAYBACK_PREPARE_MS + 100u)) {
        stopped = true;
    }

    servo_service_frame_t *prev_f = start_f;
    for (int i = 1; !stopped && i < frame_count; i++) {
        if (s_playback_stop_requested || s_current_state != SERVO_SERVICE_STATE_PLAYING) {
            stopped = true;
            break;
        }

        servo_service_frame_t *curr_f = &s_recorded_frames[slot][i];
        uint32_t time_diff_ms = (curr_f->time_ms > prev_f->time_ms)
                                    ? (curr_f->time_ms - prev_f->time_ms)
                                    : s_params.sampling_ms;
        uint32_t playback_time = (s_params.playback_speed > 0)
                                     ? (time_diff_ms * 10u / s_params.playback_speed)
                                     : time_diff_ms;
        if (playback_time == 0) playback_time = 1;

        uint32_t pos_diff = (uint32_t)abs(curr_f->em3_pos - prev_f->em3_pos);

        if (pos_diff > SERVO_SERVICE_EM3_POSITION_THRESHOLD && playback_time > 1u) {
            int16_t  mid_pos     = (int16_t)((prev_f->em3_pos + curr_f->em3_pos) / 2);
            uint32_t half_time   = playback_time / 2u;
            uint32_t second_time = playback_time - half_time;

            servo_lx_move_group(s_lx_ids, curr_f->lx_pos, SERVO_SERVICE_LX_COUNT,
                                (uint16_t)playback_time);
            servo_em3_move(SERVO_SERVICE_EM3_ID, mid_pos,
                           em3_speed_for_segment(prev_f->em3_pos, mid_pos, half_time));
            if (!playback_delay(half_time)) { stopped = true; break; }

            servo_em3_move(SERVO_SERVICE_EM3_ID, curr_f->em3_pos,
                           em3_speed_for_segment(mid_pos, curr_f->em3_pos, second_time));
            if (!playback_delay(second_time)) { stopped = true; break; }
        } else {
            servo_em3_move(SERVO_SERVICE_EM3_ID, curr_f->em3_pos,
                           em3_speed_for_segment(prev_f->em3_pos, curr_f->em3_pos, playback_time));
            servo_lx_move_group(s_lx_ids, curr_f->lx_pos, SERVO_SERVICE_LX_COUNT,
                                (uint16_t)playback_time);
            if (!playback_delay(playback_time)) { stopped = true; break; }
        }

        prev_f = curr_f;
    }

    if (stopped || s_playback_stop_requested) {
        ESP_LOGI(TAG, "Playback stopped");
    } else {
        ESP_LOGI(TAG, "Playback finished");
    }

    s_playback_stop_requested = false;
    s_current_state = (s_frame_counts[slot] > 0)
                          ? SERVO_SERVICE_STATE_HAS_DATA
                          : SERVO_SERVICE_STATE_IDLE;
    s_playback_task_handle = NULL;
    vTaskDelete(NULL);
}

/* ==================== Event Callbacks ==================== */

static void servo_service_event_handler(event_t *event, void *context)
{
    switch (event->type) {
        case EV_SERVO_MOVE: {
            /* Extract servo_id and position from event data (first byte = servo_id, next 2 = pos) */
            if (event->data && event->data_size >= 3) {
                uint8_t *d = (uint8_t *)event->data;
                uint8_t servo_id = d[0];
                uint16_t pos = (uint16_t)d[1] | ((uint16_t)d[2] << 8);
                servo_service_set_position(servo_id, pos);
            }
            break;
        }
        case EV_SERVO_SPEED_SET: {
            /* Extract servo_id and speed from event data (first byte = servo_id, next 2 = speed) */
            if (event->data && event->data_size >= 3) {
                uint8_t *d = (uint8_t *)event->data;
                uint8_t servo_id = d[0];
                uint16_t speed = (uint16_t)d[1] | ((uint16_t)d[2] << 8);
                servo_service_set_speed(servo_id, speed);
            }
            break;
        }
        case EV_SERVO_STOP:
            /* Stop all servos (disable torque) */
            servo_em3_enable_torque(SERVO_SERVICE_EM3_ID, 0);
            servo_lx_unload_all(s_lx_ids, SERVO_SERVICE_LX_COUNT);
            break;
        case EV_SERVO_POSITION_REACHED:
            ESP_LOGI(TAG, "Servo position reached event received");
            break;
        case EV_SERVO_ERROR:
            ESP_LOGE(TAG, "Servo error event received");
            break;
        case EV_SERVO_STATE_CHANGED:
            ESP_LOGI(TAG, "Servo state changed event received");
            break;
        default:
            break;
    }
}

static void servo_service_sync_device_state(void)
{
    int16_t lx_pos[SERVO_SERVICE_LX_COUNT];
    for (int i = 0; i < SERVO_SERVICE_LX_COUNT; i++) {
        lx_pos[i] = (int16_t)s_current_positions[s_lx_ids[i]];
    }
    device_state_set_servo((int16_t)s_current_positions[0], lx_pos, s_initialized);
}

static void servo_service_publish_position(uint8_t servo_id, uint16_t position)
{
    uint8_t data[3] = { servo_id, (uint8_t)(position >> 8), (uint8_t)(position & 0xFF) };

    event_t ev = {
        .type = EV_SERVO_POSITION_REACHED,
        .data = data,
        .data_size = sizeof(data),
        .timestamp = event_bus_get_timestamp(),
    };
    event_bus_publish(&ev);
}

/* ===================== Public API ===================== */

esp_err_t servo_service_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing servo service...");

    esp_err_t ret = servo_driver_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Servo driver init failed");
        return ret;
    }

    /* Initialize position cache */
    for (int i = 0; i < SERVO_COUNT; i++) {
        s_current_positions[i] = 0xFFFF;
    }

    /* Initialize EM3 and LX servo subsystems */
    servo_em3_config_t em3_cfg = {
        .uart_num = UART_NUM_1,
        .tx_pin = -1,
        .rx_pin = -1,
        .baud_rate = 115200,
    };
    servo_em3_init(&em3_cfg);

    servo_lx_config_t lx_cfg = {
        .uart_num = UART_NUM_2,
        .tx_pin = -1,
        .rx_pin = -1,
        .baud_rate = 115200,
    };
    servo_lx_init(&lx_cfg);

    /* Subscribe to servo events */
    event_bus_subscribe(EV_SERVO_MOVE, servo_service_event_handler, NULL);
    event_bus_subscribe(EV_SERVO_SPEED_SET, servo_service_event_handler, NULL);
    event_bus_subscribe(EV_SERVO_STOP, servo_service_event_handler, NULL);
    event_bus_subscribe(EV_SERVO_POSITION_REACHED, servo_service_event_handler, NULL);
    event_bus_subscribe(EV_SERVO_ERROR, servo_service_event_handler, NULL);
    event_bus_subscribe(EV_SERVO_STATE_CHANGED, servo_service_event_handler, NULL);

    s_initialized = true;
    ESP_LOGI(TAG, "Servo service initialized");
    return ESP_OK;
}

esp_err_t servo_service_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    servo_service_stop_playback();
    for (int i = 0; s_playback_task_handle != NULL && i < 50; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    servo_em3_deinit();
    servo_lx_deinit();

    s_initialized = false;
    ESP_LOGI(TAG, "Servo service deinitialized");
    return ESP_OK;
}

esp_err_t servo_service_set_position(uint8_t servo_id, uint16_t position)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (servo_id >= SERVO_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Servo %d set position: %u", servo_id, position);

    esp_err_t ret = servo_driver_set_position(servo_id, position);
    if (ret != ESP_OK) {
        return ret;
    }

    s_current_positions[servo_id] = position;
    servo_service_publish_position(servo_id, position);
    servo_service_sync_device_state();

    return ESP_OK;
}

esp_err_t servo_service_set_speed(uint8_t servo_id, uint16_t speed)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (servo_id >= SERVO_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Servo %d set speed: %u", servo_id, speed);
    return servo_driver_set_speed(servo_id, speed);
}

esp_err_t servo_service_enable(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_enabled) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Enabling servos");
    esp_err_t ret = servo_driver_enable();
    if (ret == ESP_OK) {
        s_enabled = true;
    }
    return ret;
}

esp_err_t servo_service_disable(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_enabled) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Disabling servos");
    esp_err_t ret = servo_driver_disable();
    if (ret == ESP_OK) {
        s_enabled = false;
    }
    return ret;
}

uint16_t servo_service_get_position(uint8_t servo_id)
{
    if (!s_initialized || servo_id >= SERVO_COUNT) {
        return 0xFFFF;
    }
    return s_current_positions[servo_id];
}

esp_err_t servo_service_clamp_positions(servo_service_positions_t *positions)
{
    if (positions == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (positions->em3_pos < s_limits.em3_min) positions->em3_pos = s_limits.em3_min;
    if (positions->em3_pos > s_limits.em3_max) positions->em3_pos = s_limits.em3_max;

    for (int i = 0; i < SERVO_SERVICE_LX_COUNT; i++) {
        if (positions->lx_pos[i] < s_limits.lx_min) positions->lx_pos[i] = s_limits.lx_min;
        if (positions->lx_pos[i] > s_limits.lx_max) positions->lx_pos[i] = s_limits.lx_max;
    }

    return ESP_OK;
}

esp_err_t servo_service_get_home_positions(servo_service_positions_t *positions)
{
    if (positions == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *positions = s_home_positions;
    return ESP_OK;
}

esp_err_t servo_service_set_all_positions(const servo_service_positions_t *positions, uint16_t time_ms)
{
    if (!s_initialized || positions == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    servo_service_positions_t target = *positions;
    servo_service_clamp_positions(&target);

    ESP_LOGI(TAG, "Set all servos: EM3=%d LX=[%d,%d,%d,%d] time=%dms",
             target.em3_pos, target.lx_pos[0], target.lx_pos[1],
             target.lx_pos[2], target.lx_pos[3], time_ms);

    /* Move LX servos as a group */
    servo_lx_move_group((uint8_t *)s_lx_ids, target.lx_pos, SERVO_SERVICE_LX_COUNT, time_ms);

    /* Move EM3 servo with calculated speed to reach in similar time */
    uint16_t em3_speed = SERVO_SERVICE_EM3_DEFAULT_SPEED;
    uint16_t current_em3 = s_current_positions[0];
    if (current_em3 != 0xFFFF && time_ms > 0) {
        uint32_t diff = (uint32_t)abs((int)target.em3_pos - (int)current_em3);
        uint32_t calc_speed = (diff * 1000u + time_ms - 1u) / time_ms;
        if (calc_speed < SERVO_SERVICE_EM3_MIN_SPEED) calc_speed = SERVO_SERVICE_EM3_MIN_SPEED;
        if (calc_speed > SERVO_SERVICE_EM3_MAX_SPEED) calc_speed = SERVO_SERVICE_EM3_MAX_SPEED;
        em3_speed = (uint16_t)calc_speed;
    }
    servo_em3_move(SERVO_SERVICE_EM3_ID, (uint16_t)target.em3_pos, em3_speed);

    /* Update cache */
    s_current_positions[0] = (uint16_t)target.em3_pos;
    /* We don't have per-LX cache in s_current_positions, keep unknown for indices 1 */

    return ESP_OK;
}

esp_err_t servo_service_go_home(uint16_t time_ms)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "Going home");
    return servo_service_set_all_positions(&s_home_positions, time_ms);
}

esp_err_t servo_service_smooth_move(const servo_service_positions_t *target, uint16_t time_ms)
{
    if (!s_initialized || target == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    servo_service_positions_t target_clamped = *target;
    servo_service_clamp_positions(&target_clamped);

    if (time_ms == 0 || time_ms < SERVO_SERVICE_SMOOTH_STEP_MS) {
        return servo_service_set_all_positions(&target_clamped, time_ms);
    }

    /* Read current positions */
    servo_service_positions_t start;
    int16_t em3_now = servo_em3_read_pos(SERVO_SERVICE_EM3_ID);
    start.em3_pos = em3_pos_valid(em3_now) ? em3_now : s_home_positions.em3_pos;
    for (int i = 0; i < SERVO_SERVICE_LX_COUNT; i++) {
        int16_t pos = servo_lx_read_pos(s_lx_ids[i]);
        start.lx_pos[i] = lx_pos_valid(pos) ? pos : s_home_positions.lx_pos[i];
    }

    uint32_t steps = time_ms / SERVO_SERVICE_SMOOTH_STEP_MS;
    if (steps == 0) steps = 1;

    for (uint32_t s = 1; s <= steps; s++) {
        float t = (float)s / (float)steps;

        servo_service_positions_t step_pos;
        step_pos.em3_pos = (int16_t)(start.em3_pos + (target_clamped.em3_pos - start.em3_pos) * t);
        for (int i = 0; i < SERVO_SERVICE_LX_COUNT; i++) {
            step_pos.lx_pos[i] = (int16_t)(start.lx_pos[i] +
                (target_clamped.lx_pos[i] - start.lx_pos[i]) * t);
        }

        servo_service_set_all_positions(&step_pos, SERVO_SERVICE_SMOOTH_STEP_MS);
        vTaskDelay(pdMS_TO_TICKS(SERVO_SERVICE_SMOOTH_STEP_MS));
    }

    return ESP_OK;
}

esp_err_t servo_service_set_limits(const servo_service_limits_t *limits)
{
    if (limits == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (limits->em3_min >= limits->em3_max || limits->lx_min >= limits->lx_max) {
        return ESP_ERR_INVALID_ARG;
    }
    s_limits = *limits;
    return ESP_OK;
}

esp_err_t servo_service_get_limits(servo_service_limits_t *limits)
{
    if (limits == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *limits = s_limits;
    return ESP_OK;
}

servo_service_status_t servo_service_get_status(void)
{
    servo_service_status_t status;
    status.current_state = s_current_state;
    status.current_slot  = s_current_slot;
    status.frame_count   = s_frame_counts[s_current_slot];
    return status;
}

int servo_service_get_frame_count(int slot)
{
    if (slot < 0 || slot >= (int)s_params.max_slots) {
        return 0;
    }
    return s_frame_counts[slot];
}

void servo_service_start_recording(void)
{
    if (s_current_state == SERVO_SERVICE_STATE_RECORDING ||
        s_current_state == SERVO_SERVICE_STATE_PLAYING ||
        s_playback_task_handle != NULL) {
        ESP_LOGW(TAG, "Cannot start recording in current state");
        return;
    }

    int previous_count = s_frame_counts[s_current_slot];

    /* Disable torque for free movement during recording */
    servo_lx_unload_all(s_lx_ids, SERVO_SERVICE_LX_COUNT);
    servo_em3_enable_torque(SERVO_SERVICE_EM3_ID, 0);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Read initial frame */
    servo_service_frame_t first_frame = {0};
    if (!read_current_frame(&first_frame, 0, NULL)) {
        servo_em3_enable_torque(SERVO_SERVICE_EM3_ID, 1);
        s_current_state = (previous_count > 0) ? SERVO_SERVICE_STATE_HAS_DATA
                                                : SERVO_SERVICE_STATE_IDLE;
        ESP_LOGE(TAG, "Recording not started: failed to read initial frame");
        return;
    }

    s_frame_counts[s_current_slot] = 0;
    s_recorded_frames[s_current_slot][s_frame_counts[s_current_slot]++] = first_frame;
    s_recording_start_time = xTaskGetTickCount();
    s_last_sample_time = s_recording_start_time;

    publish_state_event(s_current_state, SERVO_SERVICE_STATE_RECORDING);
    s_current_state = SERVO_SERVICE_STATE_RECORDING;

    ESP_LOGI(TAG, "[Slot %d] Recording started (EM3:%d LX:[%d,%d,%d,%d])",
             s_current_slot, first_frame.em3_pos,
             first_frame.lx_pos[0], first_frame.lx_pos[1],
             first_frame.lx_pos[2], first_frame.lx_pos[3]);
}

void servo_service_stop_recording(void)
{
    if (s_current_state != SERVO_SERVICE_STATE_RECORDING) {
        return;
    }

    servo_em3_enable_torque(SERVO_SERVICE_EM3_ID, 1);

    int valid_count = sanitize_slot_frames(s_current_slot);
    if (valid_count > 0) {
        publish_state_event(s_current_state, SERVO_SERVICE_STATE_HAS_DATA);
        s_current_state = SERVO_SERVICE_STATE_HAS_DATA;
    } else {
        publish_state_event(s_current_state, SERVO_SERVICE_STATE_IDLE);
        s_current_state = SERVO_SERVICE_STATE_IDLE;
        ESP_LOGW(TAG, "[Slot %d] Recording stopped with no valid frames", s_current_slot);
    }

    ESP_LOGI(TAG, "[Slot %d] Recording stopped (%d frames)", s_current_slot,
             s_frame_counts[s_current_slot]);
}

void servo_service_start_playback(void)
{
    if (s_current_state != SERVO_SERVICE_STATE_HAS_DATA ||
        s_frame_counts[s_current_slot] <= 0) {
        ESP_LOGW(TAG, "No data to playback");
        return;
    }

    int valid_count = sanitize_slot_frames(s_current_slot);
    if (valid_count <= 0) {
        s_current_state = SERVO_SERVICE_STATE_IDLE;
        ESP_LOGW(TAG, "No valid data to playback");
        return;
    }

    if (s_playback_task_handle != NULL) {
        ESP_LOGW(TAG, "Playback is already running");
        return;
    }

    s_playback_stop_requested = false;
    s_current_state = SERVO_SERVICE_STATE_PLAYING;
    ESP_LOGI(TAG, "[Slot %d] Playback started (%d frames, %.1fx speed)",
             s_current_slot, s_frame_counts[s_current_slot],
             s_params.playback_speed / 10.0f);

    BaseType_t task_ret = xTaskCreate(servo_playback_task, "servo_play_task", 6144,
                                      (void *)(intptr_t)s_current_slot, 5,
                                      &s_playback_task_handle);
    if (task_ret != pdPASS) {
        s_playback_task_handle = NULL;
        s_current_state = SERVO_SERVICE_STATE_HAS_DATA;
        ESP_LOGE(TAG, "Failed to create playback task");
    }
}

void servo_service_stop_playback(void)
{
    if (s_current_state != SERVO_SERVICE_STATE_PLAYING) {
        return;
    }
    s_playback_stop_requested = true;
    ESP_LOGI(TAG, "Playback stop requested");
}

void servo_service_switch_slot(int slot)
{
    if (slot < 0 || slot >= (int)s_params.max_slots) {
        ESP_LOGW(TAG, "Invalid slot: %d", slot);
        return;
    }

    if (s_current_state == SERVO_SERVICE_STATE_RECORDING ||
        s_current_state == SERVO_SERVICE_STATE_PLAYING ||
        s_playback_task_handle != NULL) {
        ESP_LOGW(TAG, "Cannot switch slot in current state");
        return;
    }

    s_current_slot = slot;
    s_current_state = (s_frame_counts[s_current_slot] > 0)
                          ? SERVO_SERVICE_STATE_HAS_DATA
                          : SERVO_SERVICE_STATE_IDLE;

    ESP_LOGI(TAG, "Switched to slot %d (%s)", s_current_slot,
             (s_current_state == SERVO_SERVICE_STATE_HAS_DATA) ? "has data" : "empty");
}

esp_err_t servo_service_set_params(const servo_service_params_t *params)
{
    if (params == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (params->sampling_ms < 10 || params->sampling_ms > 1000) {
        ESP_LOGE(TAG, "Invalid sampling_ms: %d", params->sampling_ms);
        return ESP_ERR_INVALID_ARG;
    }

    if (params->playback_speed < 1 || params->playback_speed > 100) {
        ESP_LOGE(TAG, "Invalid playback_speed: %d", params->playback_speed);
        return ESP_ERR_INVALID_ARG;
    }

    s_params = *params;
    ESP_LOGI(TAG, "Parameters updated: sampling=%d speed=%d frames=%d slots=%d",
             s_params.sampling_ms, s_params.playback_speed,
             s_params.max_frames, s_params.max_slots);
    return ESP_OK;
}

esp_err_t servo_service_get_params(servo_service_params_t *params)
{
    if (params == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *params = s_params;
    return ESP_OK;
}

#endif /* SERVO_SERVICE_ENABLE */
