#include "servo_control_module.h"

#include "servo_control_module_config.h"

#if (SERVO_CONTROL_MODULE_ENABLE == 1)

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "servo_driver.h"
#include "touch_driver.h"

#if (EVENT_BUS_ENABLE == 1)
#include "event_bus.h"
#endif

static const char* TAG = "servo_ctrl";

#if (SERVO_MODULE_LOG_ENABLE == 1)
#define SERVO_LOG_ENABLED 1
#else
#define SERVO_LOG_ENABLED 0
#endif

#define SERVO_LOGI(fmt, ...)                          \
    do                                                \
    {                                                 \
        if (SERVO_LOG_ENABLED && s_servo_log_enabled) \
            ESP_LOGI(TAG, fmt, ##__VA_ARGS__);        \
    } while (0)
#define SERVO_LOGD(fmt, ...)                          \
    do                                                \
    {                                                 \
        if (SERVO_LOG_ENABLED && s_servo_log_enabled) \
            ESP_LOGD(TAG, fmt, ##__VA_ARGS__);        \
    } while (0)

static bool                  s_initialized                           = false;
static bool                  s_servo_log_enabled                     = false;
static servo_control_state_t s_current_state                         = SERVO_CONTROL_STATE_IDLE;
static int                   s_current_slot                          = 0;
static int                   s_frame_counts[SERVO_CONTROL_MAX_SLOTS] = {0};
static uint32_t              s_recording_start_time                  = 0;
static uint32_t              s_last_sample_time                      = 0;
static bool                  s_storage_ready                         = false;
static bool                  s_littlefs_mounted_by_module            = false;
static bool                  s_have_last_good_frame                  = false;
static servo_control_frame_t s_last_good_frame                       = {
    .lx_pos  = {700, 200, 0, 350},
    .em3_pos = 900,
    .time_ms = 0,
};
static TaskHandle_t  s_playback_task_handle    = NULL;
static volatile bool s_playback_stop_requested = false;

EXT_RAM_BSS_ATTR static servo_control_frame_t s_recorded_frames[SERVO_CONTROL_MAX_SLOTS]
                                                               [SERVO_CONTROL_MAX_FRAMES];

static uint8_t s_lx_ids[SERVO_CONTROL_LX_COUNT] = {1, 2, 3, 5};

static servo_control_params_t s_params = {
    .sampling_ms    = SERVO_CONTROL_SAMPLING_MS,
    .playback_speed = SERVO_CONTROL_PLAYBACK_SPEED_MULTIPLIER,
    .max_frames     = SERVO_CONTROL_MAX_FRAMES,
    .max_slots      = SERVO_CONTROL_MAX_SLOTS,
};

#define SERVO_CONTROL_INVALID_POS (-32768)
#define SERVO_CONTROL_STORAGE_MAGIC (0x32435653u) /* SVC2 */
#define SERVO_CONTROL_STORAGE_VERSION (2u)
#define SERVO_CONTROL_PLAYBACK_PREPARE_MS 800u
#define SERVO_CONTROL_READ_RETRIES 2

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t frame_size;
    uint32_t frame_count;
    uint32_t reserved;
} servo_control_file_header_t;

typedef struct
{
    int16_t  lx_pos[4];
    int16_t  em3_pos;
    uint16_t time_ms;
} servo_control_frame_v1_t;

static bool em3_pos_valid(int16_t pos)
{
    return pos >= 0 && pos <= 3000;
}

static bool lx_pos_valid(int16_t pos)
{
    return pos >= 0 && pos <= 1000;
}

static bool frame_positions_valid(const servo_control_frame_t* frame)
{
    if (frame == NULL || !em3_pos_valid(frame->em3_pos))
    {
        return false;
    }

    for (int i = 0; i < SERVO_CONTROL_LX_COUNT; i++)
    {
        if (!lx_pos_valid(frame->lx_pos[i]))
        {
            return false;
        }
    }

    return true;
}

static uint16_t clamp_em3_speed(uint32_t speed)
{
    if (speed < SERVO_CONTROL_EM3_MIN_SPEED)
    {
        return SERVO_CONTROL_EM3_MIN_SPEED;
    }
    if (speed > SERVO_CONTROL_EM3_MAX_SPEED)
    {
        return SERVO_CONTROL_EM3_MAX_SPEED;
    }
    return (uint16_t)speed;
}

static uint16_t em3_speed_for_segment(int16_t from_pos, int16_t to_pos, uint32_t time_ms)
{
    uint32_t diff = (uint32_t)abs(to_pos - from_pos);
    if (time_ms == 0 || diff == 0)
    {
        return SERVO_CONTROL_EM3_DEFAULT_SPEED;
    }

    uint32_t speed = (diff * 1000u + time_ms - 1u) / time_ms;
    return clamp_em3_speed(speed);
}

static int16_t read_valid_em3_pos(uint8_t id)
{
    for (int retry = 0; retry < SERVO_CONTROL_READ_RETRIES; retry++)
    {
        int16_t pos = servo_em3_read_pos(id);
        if (em3_pos_valid(pos))
        {
            return pos;
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    return SERVO_CONTROL_INVALID_POS;
}

static int16_t read_valid_lx_pos(uint8_t id)
{
    for (int retry = 0; retry < SERVO_CONTROL_READ_RETRIES; retry++)
    {
        int16_t pos = servo_lx_read_pos(id);
        if (lx_pos_valid(pos))
        {
            return pos;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return SERVO_CONTROL_INVALID_POS;
}

static bool read_current_frame(servo_control_frame_t* frame, uint32_t time_ms,
                               const servo_control_frame_t* fallback)
{
    if (frame == NULL)
    {
        return false;
    }

    bool valid = true;
    memset(frame, 0, sizeof(*frame));
    frame->time_ms = time_ms;

    int16_t em3_pos = read_valid_em3_pos(SERVO_CONTROL_EM3_ID);
    if (!em3_pos_valid(em3_pos))
    {
        if (fallback && em3_pos_valid(fallback->em3_pos))
        {
            em3_pos = fallback->em3_pos;
        }
        else
        {
            valid = false;
        }
    }
    frame->em3_pos = em3_pos;

    for (int i = 0; i < SERVO_CONTROL_LX_COUNT; i++)
    {
        int16_t lx_pos = read_valid_lx_pos(s_lx_ids[i]);
        if (!lx_pos_valid(lx_pos))
        {
            if (fallback && lx_pos_valid(fallback->lx_pos[i]))
            {
                lx_pos = fallback->lx_pos[i];
            }
            else
            {
                valid = false;
            }
        }
        frame->lx_pos[i] = lx_pos;
    }

    if (valid && frame_positions_valid(frame))
    {
        s_last_good_frame      = *frame;
        s_have_last_good_frame = true;
        return true;
    }

    return false;
}

static bool sanitize_frame(servo_control_frame_t* frame, const servo_control_frame_t* fallback)
{
    if (frame == NULL)
    {
        return false;
    }

    if (!em3_pos_valid(frame->em3_pos))
    {
        if (fallback && em3_pos_valid(fallback->em3_pos))
        {
            frame->em3_pos = fallback->em3_pos;
        }
        else
        {
            return false;
        }
    }

    for (int i = 0; i < SERVO_CONTROL_LX_COUNT; i++)
    {
        if (!lx_pos_valid(frame->lx_pos[i]))
        {
            if (fallback && lx_pos_valid(fallback->lx_pos[i]))
            {
                frame->lx_pos[i] = fallback->lx_pos[i];
            }
            else
            {
                return false;
            }
        }
    }

    return true;
}

static int sanitize_slot_frames(int slot)
{
    if (slot < 0 || slot >= SERVO_CONTROL_MAX_SLOTS)
    {
        return 0;
    }

    int count = s_frame_counts[slot];
    if (count <= 0 || count > SERVO_CONTROL_MAX_FRAMES)
    {
        s_frame_counts[slot] = 0;
        return 0;
    }

    int                   out       = 0;
    servo_control_frame_t prev      = {0};
    bool                  have_prev = false;

    for (int i = 0; i < count; i++)
    {
        servo_control_frame_t frame = s_recorded_frames[slot][i];
        if (!sanitize_frame(&frame, have_prev ? &prev : NULL))
        {
            ESP_LOGW(TAG, "Dropping invalid frame %d in slot %d", i, slot);
            continue;
        }

        if (out == 0)
        {
            frame.time_ms = 0;
        }
        else if (frame.time_ms <= prev.time_ms)
        {
            frame.time_ms = prev.time_ms + s_params.sampling_ms;
        }

        s_recorded_frames[slot][out++] = frame;
        prev                           = frame;
        have_prev                      = true;
    }

    s_frame_counts[slot] = out;
    return out;
}

static bool playback_delay(uint32_t ms)
{
    uint32_t elapsed = 0;
    while (elapsed < ms)
    {
        if (s_playback_stop_requested || s_current_state != SERVO_CONTROL_STATE_PLAYING)
        {
            return false;
        }
        uint32_t chunk = (ms - elapsed > 20u) ? 20u : (ms - elapsed);
        vTaskDelay(pdMS_TO_TICKS(chunk));
        elapsed += chunk;
    }
    return true;
}

static void load_slot_from_littlefs(int slot)
{
    char path[32];
    snprintf(path, sizeof(path), "/littlefs/slot%d.bin", slot);

    FILE* f = fopen(path, "rb");
    if (f == NULL)
    {
        s_frame_counts[slot] = 0;
        return;
    }

    s_frame_counts[slot]                    = 0;
    servo_control_file_header_t header      = {0};
    size_t                      header_read = fread(&header, sizeof(header), 1, f);

    if (header_read == 1 && header.magic == SERVO_CONTROL_STORAGE_MAGIC &&
        header.version == SERVO_CONTROL_STORAGE_VERSION &&
        header.frame_size == sizeof(servo_control_frame_t) &&
        header.frame_count <= (uint32_t)s_params.max_frames &&
        header.frame_count <= SERVO_CONTROL_MAX_FRAMES)
    {
        size_t frames_read =
            fread(s_recorded_frames[slot], sizeof(servo_control_frame_t), header.frame_count, f);
        if (frames_read == header.frame_count)
        {
            s_frame_counts[slot] = (int)header.frame_count;
        }
        else
        {
            ESP_LOGW(TAG, "Slot %d file is truncated (%u/%u frames)", slot, (unsigned)frames_read,
                     (unsigned)header.frame_count);
        }
    }
    else
    {
        rewind(f);
        int old_count = 0;
        if (fread(&old_count, sizeof(int), 1, f) == 1 && old_count > 0 &&
            old_count <= s_params.max_frames && old_count <= SERVO_CONTROL_MAX_FRAMES)
        {
            int loaded = 0;
            for (int i = 0; i < old_count; i++)
            {
                servo_control_frame_v1_t old_frame = {0};
                if (fread(&old_frame, sizeof(old_frame), 1, f) != 1)
                {
                    break;
                }

                servo_control_frame_t* dst = &s_recorded_frames[slot][loaded++];
                for (int j = 0; j < SERVO_CONTROL_LX_COUNT; j++)
                {
                    dst->lx_pos[j] = old_frame.lx_pos[j];
                }
                dst->em3_pos = old_frame.em3_pos;
                dst->time_ms = old_frame.time_ms;
            }
            s_frame_counts[slot] = loaded;
        }
        else if (old_count != 0)
        {
            ESP_LOGW(TAG, "Ignoring invalid slot %d frame count: %d", slot, old_count);
        }
    }

    fclose(f);

    int valid_count = sanitize_slot_frames(slot);
    if (valid_count > 0)
    {
        ESP_LOGI(TAG, "Loaded slot %d from Flash (%d frames)", slot, valid_count);
    }
}

static void init_littlefs(void)
{
    ESP_LOGI(TAG, "Free heap before LittleFS: %d bytes (internal: %d, psram: %d)",
             esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    esp_vfs_littlefs_conf_t conf = {
        .base_path              = "/littlefs",
        .partition_label        = "storage",
        .format_if_mount_failed = true,
        .dont_mount             = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret == ESP_OK)
    {
        s_storage_ready              = true;
        s_littlefs_mounted_by_module = true;
        ESP_LOGI(TAG, "LittleFS mounted successfully");
    }
    else if (ret == ESP_ERR_INVALID_STATE)
    {
        s_storage_ready              = true;
        s_littlefs_mounted_by_module = false;
        ESP_LOGW(TAG, "LittleFS storage partition is already mounted, reusing /littlefs");
    }
    else
    {
        ESP_LOGE(TAG, "Failed to mount LittleFS partition: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "Free heap after failure: %d bytes (internal: %d, psram: %d)",
                 esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

        if (ret == ESP_ERR_NO_MEM)
        {
            ESP_LOGI(TAG, "Attempting to format LittleFS partition...");
            ret = esp_littlefs_format("storage");
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to format LittleFS partition: %s", esp_err_to_name(ret));
                return;
            }

            ESP_LOGI(TAG, "LittleFS partition formatted, attempting to mount again...");
            ret = esp_vfs_littlefs_register(&conf);
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to mount LittleFS partition after format: %s",
                         esp_err_to_name(ret));
                return;
            }
            s_storage_ready              = true;
            s_littlefs_mounted_by_module = true;
        }
        else
        {
            return;
        }
    }

    ESP_LOGI(TAG, "Loading recorded data from Flash...");
    for (int slot = 0; slot < SERVO_CONTROL_MAX_SLOTS; slot++)
    {
        load_slot_from_littlefs(slot);
    }
    ESP_LOGI(TAG, "Data load finished");
}

static void save_to_littlefs(int slot)
{
    if (slot < 0 || slot >= SERVO_CONTROL_MAX_SLOTS)
    {
        return;
    }

    int valid_count = sanitize_slot_frames(slot);
    if (valid_count <= 0)
    {
        ESP_LOGW(TAG, "Slot %d has no valid frames to save", slot);
        return;
    }

    if (!s_storage_ready)
    {
        ESP_LOGE(TAG, "Cannot save slot %d: LittleFS is not ready", slot);
        return;
    }

    char path[32];
    snprintf(path, sizeof(path), "/littlefs/slot%d.bin", slot);

    FILE* f = fopen(path, "wb");
    if (f == NULL)
    {
        ESP_LOGE(TAG, "Failed to save slot %d: cannot open file for writing", slot);
        ESP_LOGE(TAG, "Path: %s, errno: %d", path, errno);
        return;
    }

    servo_control_file_header_t header = {
        .magic       = SERVO_CONTROL_STORAGE_MAGIC,
        .version     = SERVO_CONTROL_STORAGE_VERSION,
        .frame_size  = sizeof(servo_control_frame_t),
        .frame_count = (uint32_t)valid_count,
        .reserved    = 0,
    };

    size_t header_written = fwrite(&header, sizeof(header), 1, f);
    size_t frames_written =
        fwrite(s_recorded_frames[slot], sizeof(servo_control_frame_t), valid_count, f);
    int close_ret = fclose(f);

    if (header_written != 1 || frames_written != (size_t)valid_count || close_ret != 0)
    {
        ESP_LOGE(TAG, "Failed to fully save slot %d (%u/%d frames, close=%d)", slot,
                 (unsigned)frames_written, valid_count, close_ret);
        return;
    }

    ESP_LOGI(TAG, "Slot %d saved to Flash (%d frames)", slot, valid_count);
}

#if (EVENT_BUS_ENABLE == 1)
static void publish_servo_mode_event(servo_control_state_t old_state,
                                     servo_control_state_t new_state)
{
    servo_mode_data_t mode_data = {
        .servo_id = 0,
        .old_mode = (servo_mode_t)old_state,
        .new_mode = (servo_mode_t)new_state,
    };
    event_bus_publish_simple(EVENT_TYPE_SERVO_CONTROL, SERVO_EVENT_MODE_CHANGED, &mode_data,
                             sizeof(mode_data));
}
#endif

esp_err_t servo_control_init(void)
{
    if (s_initialized)
    {
        ESP_LOGW(TAG, "Servo control already initialized");
        return ESP_OK;
    }

    init_littlefs();

    if (s_frame_counts[s_current_slot] > 0)
    {
        s_current_state = SERVO_CONTROL_STATE_HAS_DATA;
    }

#if (EVENT_BUS_ENABLE == 1)
    if (old_state != s_current_state)
    {
        publish_servo_mode_event(old_state, s_current_state);
    }
#endif

    s_initialized = true;

    s_servo_log_enabled = false;

    ESP_LOGI(TAG, "Servo control initialized successfully");

    return ESP_OK;
}

esp_err_t servo_control_deinit(void)
{
    if (!s_initialized)
    {
        ESP_LOGW(TAG, "Servo control not initialized");
        return ESP_OK;
    }

    servo_control_stop_playback();
    for (int i = 0; s_playback_task_handle != NULL && i < 50; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (s_littlefs_mounted_by_module)
    {
        esp_err_t ret = esp_vfs_littlefs_unregister("storage");
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "Failed to unmount LittleFS: %s", esp_err_to_name(ret));
        }
    }

    s_storage_ready              = false;
    s_littlefs_mounted_by_module = false;
    s_initialized                = false;
    ESP_LOGI(TAG, "Servo control deinitialized successfully");

    return ESP_OK;
}

servo_control_status_t servo_control_get_status(void)
{
    servo_control_status_t status;
    status.current_state = s_current_state;
    status.current_slot  = s_current_slot;
    status.frame_count   = s_frame_counts[s_current_slot];
    return status;
}

int servo_control_get_frame_count(int slot)
{
    if (slot < 0 || slot >= s_params.max_slots)
    {
        return 0;
    }
    return s_frame_counts[slot];
}

void servo_control_start_recording(void)
{
    if (s_current_state == SERVO_CONTROL_STATE_RECORDING ||
        s_current_state == SERVO_CONTROL_STATE_PLAYING || s_playback_task_handle != NULL)
    {
        ESP_LOGW(TAG, "Cannot start recording in current state");
        return;
    }

    int previous_count = s_frame_counts[s_current_slot];

    servo_lx_unload_all(s_lx_ids, SERVO_CONTROL_LX_COUNT);
    servo_em3_enable_torque(SERVO_CONTROL_EM3_ID, 0);

    vTaskDelay(pdMS_TO_TICKS(100));

    servo_control_frame_t first_frame = {0};
    if (!read_current_frame(&first_frame, 0, NULL))
    {
        servo_em3_enable_torque(SERVO_CONTROL_EM3_ID, 1);
#if (EVENT_BUS_ENABLE == 1)
        publish_servo_mode_event(s_current_state, s_current_state);
#endif
        s_current_state =
            (previous_count > 0) ? SERVO_CONTROL_STATE_HAS_DATA : SERVO_CONTROL_STATE_IDLE;
        ESP_LOGE(TAG, "Recording not started: failed to read a complete valid initial frame");
        return;
    }

    s_frame_counts[s_current_slot]                                      = 0;
    s_recorded_frames[s_current_slot][s_frame_counts[s_current_slot]++] = first_frame;
    s_recording_start_time                                              = xTaskGetTickCount();
    s_last_sample_time                                                  = s_recording_start_time;

#if (EVENT_BUS_ENABLE == 1)
    publish_servo_mode_event(s_current_state, SERVO_CONTROL_STATE_RECORDING);
#endif
    s_current_state = SERVO_CONTROL_STATE_RECORDING;

    SERVO_LOGI("[Slot %d] Recording started (initial frame: EM3:%4d | ID1:%4d | ID2:%4d | ID3:%4d "
               "| ID5:%4d)",
               s_current_slot, first_frame.em3_pos, first_frame.lx_pos[0], first_frame.lx_pos[1],
               first_frame.lx_pos[2], first_frame.lx_pos[3]);
}

void servo_control_stop_recording(void)
{
    if (s_current_state != SERVO_CONTROL_STATE_RECORDING)
    {
        return;
    }

    servo_em3_enable_torque(SERVO_CONTROL_EM3_ID, 1);

    int valid_count = sanitize_slot_frames(s_current_slot);
    if (valid_count > 0)
    {
#if (EVENT_BUS_ENABLE == 1)
        publish_servo_mode_event(s_current_state, SERVO_CONTROL_STATE_HAS_DATA);
#endif
        s_current_state = SERVO_CONTROL_STATE_HAS_DATA;
        save_to_littlefs(s_current_slot);
    }
    else
    {
#if (EVENT_BUS_ENABLE == 1)
        publish_servo_mode_event(s_current_state, SERVO_CONTROL_STATE_IDLE);
#endif
        s_current_state = SERVO_CONTROL_STATE_IDLE;
        ESP_LOGW(TAG, "[Slot %d] Recording stopped with no valid frames", s_current_slot);
    }

    SERVO_LOGI("[Slot %d] Recording stopped (%d frames)", s_current_slot,
               s_frame_counts[s_current_slot]);
}

#if 0
void servo_control_start_playback(void)
{
    if (s_current_state != SERVO_CONTROL_STATE_HAS_DATA) {
        ESP_LOGW(TAG, "No data to playback");
        return;
    }

    s_current_state = SERVO_CONTROL_STATE_PLAYING;
    SERVO_LOGI("[Slot %d] Playback started (%d frames, %.1fx speed)", 
             s_current_slot, s_frame_counts[s_current_slot], 
             s_params.playback_speed / 10.0f);

    servo_control_frame_t *start_f = &s_recorded_frames[s_current_slot][0];
    uint16_t initial_time = start_f->time_ms * s_params.playback_speed / 10;
    servo_em3_move(SERVO_CONTROL_EM3_ID, start_f->em3_pos, SERVO_CONTROL_EM3_DEFAULT_SPEED);
    servo_lx_move_group(s_lx_ids, start_f->lx_pos, SERVO_CONTROL_LX_COUNT, initial_time);
    vTaskDelay(pdMS_TO_TICKS(initial_time + 100));

    servo_control_frame_t *prev_f = start_f;
    for (int i = 1; i < s_frame_counts[s_current_slot]; i++) {
        if (s_current_state != SERVO_CONTROL_STATE_PLAYING) {
            SERVO_LOGI("Playback stopped");
            break;
        }

        servo_control_frame_t *curr_f = &s_recorded_frames[s_current_slot][i];
        uint16_t time_diff_ms = curr_f->time_ms - prev_f->time_ms;
        uint16_t playback_time = (s_params.playback_speed > 0) ? (time_diff_ms * 10 / s_params.playback_speed) : time_diff_ms;
        
        if (i % 10 == 0) {
            SERVO_LOGI("[Playback] Frame %d/%d, Time: %d ms, EM3: %d, LX: [%d, %d, %d, %d]",
                     i, s_frame_counts[s_current_slot], curr_f->time_ms,
                     curr_f->em3_pos, curr_f->lx_pos[0], curr_f->lx_pos[1], 
                     curr_f->lx_pos[2], curr_f->lx_pos[3]);
        }
        
        int16_t pos_diff = abs(curr_f->em3_pos - prev_f->em3_pos);
        
        // TODO: [High Priority] EM3舵机倒转问题
        // 问题：EM3舵机是位置舵机，有角度限制（0-3000），当位置接近边界时，舵机会选择最短路径移动，导致倒转
        // 当前方案：分步移动，当位置变化超过阈值时，分两步移动
        // 改进方案：改为速度模式控制，避免位置限制导致的倒转问题
        // 相关配置：SERVO_CONTROL_EM3_POSITION_THRESHOLD
        if (pos_diff > SERVO_CONTROL_EM3_POSITION_THRESHOLD) {
            uint16_t mid_pos = (prev_f->em3_pos + curr_f->em3_pos) / 2;
            uint16_t half_time = playback_time / 2;
            
            float half_time_s = half_time / 1000.0f;
            uint16_t em3_speed1 = (half_time_s > 0) ? (uint16_t)(abs(mid_pos - prev_f->em3_pos) / half_time_s) : SERVO_CONTROL_EM3_DEFAULT_SPEED;
            if (em3_speed1 < SERVO_CONTROL_EM3_MIN_SPEED) em3_speed1 = SERVO_CONTROL_EM3_MIN_SPEED;
            else if (em3_speed1 > SERVO_CONTROL_EM3_MAX_SPEED) em3_speed1 = SERVO_CONTROL_EM3_MAX_SPEED;
            
            servo_em3_move(SERVO_CONTROL_EM3_ID, mid_pos, em3_speed1);
            servo_lx_move_group(s_lx_ids, curr_f->lx_pos, SERVO_CONTROL_LX_COUNT, half_time);
            vTaskDelay(pdMS_TO_TICKS(half_time));
            
            uint16_t em3_speed2 = (half_time_s > 0) ? (uint16_t)(abs(curr_f->em3_pos - mid_pos) / half_time_s) : SERVO_CONTROL_EM3_DEFAULT_SPEED;
            if (em3_speed2 < SERVO_CONTROL_EM3_MIN_SPEED) em3_speed2 = SERVO_CONTROL_EM3_MIN_SPEED;
            else if (em3_speed2 > SERVO_CONTROL_EM3_MAX_SPEED) em3_speed2 = SERVO_CONTROL_EM3_MAX_SPEED;
            
            servo_em3_move(SERVO_CONTROL_EM3_ID, curr_f->em3_pos, em3_speed2);
            vTaskDelay(pdMS_TO_TICKS(half_time));
        } else {
            float time_diff_s = playback_time / 1000.0f;
            uint16_t em3_speed = (time_diff_s > 0) ? (uint16_t)(pos_diff / time_diff_s) : SERVO_CONTROL_EM3_DEFAULT_SPEED;
            
            if (em3_speed < SERVO_CONTROL_EM3_MIN_SPEED) {
                em3_speed = SERVO_CONTROL_EM3_MIN_SPEED;
            } else if (em3_speed > SERVO_CONTROL_EM3_MAX_SPEED) {
                em3_speed = SERVO_CONTROL_EM3_MAX_SPEED;
            }
            
            servo_em3_move(SERVO_CONTROL_EM3_ID, curr_f->em3_pos, em3_speed);
            servo_lx_move_group(s_lx_ids, curr_f->lx_pos, SERVO_CONTROL_LX_COUNT, playback_time);
            vTaskDelay(pdMS_TO_TICKS(playback_time));
        }
        
        prev_f = curr_f;
    }

    SERVO_LOGI("Playback finished");
    s_current_state = SERVO_CONTROL_STATE_HAS_DATA;
}

void servo_control_stop_playback(void)
{
    if (s_current_state != SERVO_CONTROL_STATE_PLAYING) {
        return;
    }

    s_current_state = SERVO_CONTROL_STATE_HAS_DATA;
    SERVO_LOGI("Playback stopped");
}

#endif

static void servo_control_playback_task(void* arg)
{
    int  slot        = (int)(intptr_t)arg;
    int  frame_count = s_frame_counts[slot];
    bool stopped     = false;

    servo_em3_enable_torque(SERVO_CONTROL_EM3_ID, 1);

    servo_control_frame_t* start_f = &s_recorded_frames[slot][0];
    servo_em3_move(SERVO_CONTROL_EM3_ID, start_f->em3_pos, SERVO_CONTROL_EM3_DEFAULT_SPEED);
    servo_lx_move_group(s_lx_ids, start_f->lx_pos, SERVO_CONTROL_LX_COUNT,
                        SERVO_CONTROL_PLAYBACK_PREPARE_MS);
    if (!playback_delay(SERVO_CONTROL_PLAYBACK_PREPARE_MS + 100u))
    {
        stopped = true;
    }

    servo_control_frame_t* prev_f = start_f;
    for (int i = 1; !stopped && i < frame_count; i++)
    {
        if (s_playback_stop_requested || s_current_state != SERVO_CONTROL_STATE_PLAYING)
        {
            stopped = true;
            break;
        }

        servo_control_frame_t* curr_f        = &s_recorded_frames[slot][i];
        uint32_t               time_diff_ms  = (curr_f->time_ms > prev_f->time_ms)
                                                   ? (curr_f->time_ms - prev_f->time_ms)
                                                   : s_params.sampling_ms;
        uint32_t               playback_time = (s_params.playback_speed > 0)
                                                   ? (time_diff_ms * 10u / s_params.playback_speed)
                                                   : time_diff_ms;
        if (playback_time == 0)
        {
            playback_time = 1;
        }

        uint16_t lx_time = (playback_time > UINT16_MAX) ? UINT16_MAX : (uint16_t)playback_time;

        if (i % 10 == 0)
        {
            SERVO_LOGI("[Playback] Frame %d/%d, Time: %lu ms, EM3: %d, LX: [%d, %d, %d, %d]", i,
                       frame_count, (unsigned long)curr_f->time_ms, curr_f->em3_pos,
                       curr_f->lx_pos[0], curr_f->lx_pos[1], curr_f->lx_pos[2], curr_f->lx_pos[3]);
        }

        uint32_t pos_diff = (uint32_t)abs(curr_f->em3_pos - prev_f->em3_pos);

        if (pos_diff > SERVO_CONTROL_EM3_POSITION_THRESHOLD && playback_time > 1u)
        {
            int16_t  mid_pos     = (int16_t)((prev_f->em3_pos + curr_f->em3_pos) / 2);
            uint32_t half_time   = playback_time / 2u;
            uint32_t second_time = playback_time - half_time;

            servo_lx_move_group(s_lx_ids, curr_f->lx_pos, SERVO_CONTROL_LX_COUNT, lx_time);
            servo_em3_move(SERVO_CONTROL_EM3_ID, mid_pos,
                           em3_speed_for_segment(prev_f->em3_pos, mid_pos, half_time));
            if (!playback_delay(half_time))
            {
                stopped = true;
                break;
            }

            servo_em3_move(SERVO_CONTROL_EM3_ID, curr_f->em3_pos,
                           em3_speed_for_segment(mid_pos, curr_f->em3_pos, second_time));
            if (!playback_delay(second_time))
            {
                stopped = true;
                break;
            }
        }
        else
        {
            servo_em3_move(SERVO_CONTROL_EM3_ID, curr_f->em3_pos,
                           em3_speed_for_segment(prev_f->em3_pos, curr_f->em3_pos, playback_time));
            servo_lx_move_group(s_lx_ids, curr_f->lx_pos, SERVO_CONTROL_LX_COUNT, lx_time);
            if (!playback_delay(playback_time))
            {
                stopped = true;
                break;
            }
        }

        prev_f = curr_f;
    }

    if (stopped || s_playback_stop_requested)
    {
        SERVO_LOGI("Playback stopped");
    }
    else
    {
        SERVO_LOGI("Playback finished");
    }

    s_playback_stop_requested = false;
    s_current_state =
        (s_frame_counts[slot] > 0) ? SERVO_CONTROL_STATE_HAS_DATA : SERVO_CONTROL_STATE_IDLE;
    s_playback_task_handle = NULL;
    vTaskDelete(NULL);
}

void servo_control_start_playback(void)
{
    if (s_current_state != SERVO_CONTROL_STATE_HAS_DATA || s_frame_counts[s_current_slot] <= 0)
    {
        ESP_LOGW(TAG, "No data to playback");
        return;
    }

    int valid_count = sanitize_slot_frames(s_current_slot);
    if (valid_count <= 0)
    {
        s_current_state = SERVO_CONTROL_STATE_IDLE;
        ESP_LOGW(TAG, "No valid data to playback");
        return;
    }

    if (s_playback_task_handle != NULL)
    {
        ESP_LOGW(TAG, "Playback is already running");
        return;
    }

    s_playback_stop_requested = false;
    s_current_state           = SERVO_CONTROL_STATE_PLAYING;
    SERVO_LOGI("[Slot %d] Playback started (%d frames, %.1fx speed)", s_current_slot,
               s_frame_counts[s_current_slot], s_params.playback_speed / 10.0f);

    BaseType_t task_ret = xTaskCreate(servo_control_playback_task, "servo_play_task", 6144,
                                      (void*)(intptr_t)s_current_slot, 5, &s_playback_task_handle);
    if (task_ret != pdPASS)
    {
        s_playback_task_handle = NULL;
        s_current_state        = SERVO_CONTROL_STATE_HAS_DATA;
        ESP_LOGE(TAG, "Failed to create playback task");
    }
}

void servo_control_stop_playback(void)
{
    if (s_current_state != SERVO_CONTROL_STATE_PLAYING)
    {
        return;
    }

    s_playback_stop_requested = true;
    SERVO_LOGI("Playback stop requested");
}

void servo_control_switch_slot(int slot)
{
    if (slot < 0 || slot >= s_params.max_slots)
    {
        ESP_LOGW(TAG, "Invalid slot: %d", slot);
        return;
    }

    if (s_current_state == SERVO_CONTROL_STATE_RECORDING ||
        s_current_state == SERVO_CONTROL_STATE_PLAYING || s_playback_task_handle != NULL)
    {
        ESP_LOGW(TAG, "Cannot switch slot in current state");
        return;
    }

    s_current_slot = slot;
    if (s_frame_counts[s_current_slot] > 0)
    {
        s_current_state = SERVO_CONTROL_STATE_HAS_DATA;
    }
    else
    {
        s_current_state = SERVO_CONTROL_STATE_IDLE;
    }

    SERVO_LOGI("Switched to slot %d (%s)", s_current_slot,
               (s_current_state == SERVO_CONTROL_STATE_HAS_DATA) ? "has data" : "empty");
}

void servo_control_task(void* arg)
{
    SERVO_LOGI("Servo control task started");

    bool     last_touch_a = false;
    bool     last_touch_b = false;
    bool     last_touch_c = false;
    uint32_t last_monitor = 0;

    while (1)
    {
        uint32_t now_tick = xTaskGetTickCount();

        touch_driver_scan();
        bool touch_a = touch_driver_get_state(TOUCH_POINT_A);
        bool touch_b = touch_driver_get_state(TOUCH_POINT_B);
        bool touch_c = touch_driver_get_state(TOUCH_POINT_C);

        if (s_current_state == SERVO_CONTROL_STATE_IDLE ||
            s_current_state == SERVO_CONTROL_STATE_HAS_DATA)
        {
            if (now_tick - last_monitor >= pdMS_TO_TICKS(1000))
            {
                int16_t p_em3 = servo_em3_read_pos(SERVO_CONTROL_EM3_ID);
                int16_t p1    = servo_lx_read_pos(s_lx_ids[0]);
                vTaskDelay(pdMS_TO_TICKS(5));
                int16_t p2 = servo_lx_read_pos(s_lx_ids[1]);
                vTaskDelay(pdMS_TO_TICKS(5));
                int16_t p3 = servo_lx_read_pos(s_lx_ids[2]);
                vTaskDelay(pdMS_TO_TICKS(5));
                int16_t p5 = servo_lx_read_pos(s_lx_ids[3]);

                if (p_em3 != SERVO_CONTROL_INVALID_POS || p1 != SERVO_CONTROL_INVALID_POS ||
                    p2 != SERVO_CONTROL_INVALID_POS || p3 != SERVO_CONTROL_INVALID_POS ||
                    p5 != SERVO_CONTROL_INVALID_POS)
                {
                    // SERVO_LOGI("[LIVE] EM3:%4d | ID1:%4d | ID2:%4d | ID3:%4d | ID5:%4d",
                    //          p_em3, p1, p2, p3, p5);
                }
                last_monitor = now_tick;
            }
        }

        if (touch_a && !last_touch_a)
        {
            if (s_current_state != SERVO_CONTROL_STATE_RECORDING &&
                s_current_state != SERVO_CONTROL_STATE_PLAYING)
            {
                servo_control_start_recording();
            }
            else if (s_current_state == SERVO_CONTROL_STATE_RECORDING)
            {
                servo_control_stop_recording();
            }
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        last_touch_a = touch_a;

        if (touch_c && !last_touch_c)
        {
            if (s_current_state != SERVO_CONTROL_STATE_RECORDING &&
                s_current_state != SERVO_CONTROL_STATE_PLAYING)
            {
                int next_slot = (s_current_slot + 1) % s_params.max_slots;
                servo_control_switch_slot(next_slot);
            }
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        last_touch_c = touch_c;

        if (s_current_state == SERVO_CONTROL_STATE_RECORDING)
        {
            uint32_t now = xTaskGetTickCount();
            if (now - s_last_sample_time >= pdMS_TO_TICKS(s_params.sampling_ms))
            {
                if (s_frame_counts[s_current_slot] < s_params.max_frames)
                {
                    servo_control_frame_t        frame = {0};
                    const servo_control_frame_t* fallback =
                        s_have_last_good_frame ? &s_last_good_frame : NULL;
                    uint32_t elapsed_ms = (now - s_recording_start_time) * portTICK_PERIOD_MS;
                    if (read_current_frame(&frame, elapsed_ms, fallback))
                    {
                        s_recorded_frames[s_current_slot][s_frame_counts[s_current_slot]++] = frame;

                        if (s_frame_counts[s_current_slot] % 4 == 0)
                        {
                            SERVO_LOGI("[REC %03d] EM3:%4d | ID1:%4d | ID2:%4d | ID3:%4d | ID5:%4d "
                                       "| Time:%lums",
                                       s_frame_counts[s_current_slot], frame.em3_pos,
                                       frame.lx_pos[0], frame.lx_pos[1], frame.lx_pos[2],
                                       frame.lx_pos[3], (unsigned long)frame.time_ms);
                        }
                    }
                    else
                    {
                        ESP_LOGW(
                            TAG,
                            "[Slot %d] Skipped sample: no valid fallback for servo read failure",
                            s_current_slot);
                    }
                }
                else
                {
                    servo_control_stop_recording();
                    SERVO_LOGI("[Slot %d] Recording limit reached, auto-saved", s_current_slot);
                }
                s_last_sample_time = now;
            }
        }

        if (touch_b && !last_touch_b && s_current_state == SERVO_CONTROL_STATE_HAS_DATA)
        {
            servo_control_start_playback();
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        last_touch_b = touch_b;

        STACK_MONITOR_LOG("servo_ctrl", 8192);

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void servo_control_enable_log(void)
{
#if (SERVO_LOG_ENABLED == 1)
    s_servo_log_enabled = true;
    ESP_LOGI(TAG, "Servo control log enabled");
#else
    ESP_LOGW(TAG, "Servo control log is disabled in Kconfig");
#endif
}

esp_err_t servo_control_set_params(servo_control_params_t* params)
{
    if (params == NULL)
    {
        ESP_LOGE(TAG, "Invalid params pointer");
        return ESP_ERR_INVALID_ARG;
    }

    if (params->sampling_ms < 10 || params->sampling_ms > 1000)
    {
        ESP_LOGE(TAG, "Invalid sampling_ms: %d (range: 10-1000)", params->sampling_ms);
        return ESP_ERR_INVALID_ARG;
    }

    if (params->playback_speed < 1 || params->playback_speed > 100)
    {
        ESP_LOGE(TAG, "Invalid playback_speed: %d (range: 1-100)", params->playback_speed);
        return ESP_ERR_INVALID_ARG;
    }

    if (params->max_frames < 10 || params->max_frames > SERVO_CONTROL_MAX_FRAMES)
    {
        ESP_LOGE(TAG, "Invalid max_frames: %d (range: 10-%d)", params->max_frames,
                 SERVO_CONTROL_MAX_FRAMES);
        return ESP_ERR_INVALID_ARG;
    }

    if (params->max_slots < 1 || params->max_slots > SERVO_CONTROL_MAX_SLOTS)
    {
        ESP_LOGE(TAG, "Invalid max_slots: %d (range: 1-%d)", params->max_slots,
                 SERVO_CONTROL_MAX_SLOTS);
        return ESP_ERR_INVALID_ARG;
    }

    s_params = *params;

    ESP_LOGI(TAG, "Parameters updated:");
    ESP_LOGI(TAG, "  sampling_ms: %d", s_params.sampling_ms);
    ESP_LOGI(TAG, "  playback_speed: %d (%.1fx)", s_params.playback_speed,
             s_params.playback_speed / 10.0f);
    ESP_LOGI(TAG, "  max_frames: %d", s_params.max_frames);
    ESP_LOGI(TAG, "  max_slots: %d", s_params.max_slots);

    return ESP_OK;
}

esp_err_t servo_control_get_params(servo_control_params_t* params)
{
    if (params == NULL)
    {
        ESP_LOGE(TAG, "Invalid params pointer");
        return ESP_ERR_INVALID_ARG;
    }

    *params = s_params;
    return ESP_OK;
}

void servo_control_disable_log(void)
{
#if (SERVO_LOG_ENABLED == 1)
    s_servo_log_enabled = false;
    ESP_LOGI(TAG, "Servo control log disabled");
#else
    ESP_LOGW(TAG, "Servo control log is disabled in Kconfig");
#endif
}

bool servo_control_is_log_enabled(void)
{
    return s_servo_log_enabled;
}

#endif
