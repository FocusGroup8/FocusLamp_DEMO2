/*
 * radar_ld6002.c - Self-contained HLK-LD6002 radar reader implementation
 *
 * Migrated from the standalone radar test (radar/main/main.c).
 *
 * Frame format (HLK-LD6002):
 *   [0] SOF(0x01) | [1-2] ID(BE) | [3-4] LEN(BE) | [5-6] TYPE(BE)
 *   | [7] HCK(~XOR[0..6]) | [8..8+LEN-1] DATA | [8+LEN] DCK(~XOR[DATA])
 *
 * Uses the shared radar_driver UART layer. Independent from the
 * radar_module / radar_frame_handler / event_bus pipeline.
 */

#include "radar_ld6002.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"

#include "driver/uart.h"

#include "radar_driver.h"
#include "system_config.h"

#include "event_bus.h"
#include "event_def.h"
#include "device_state.h"
#include "freertos/semphr.h"

static const char *TAG = "radar_ld6002";

/* ===================== EMA Filter Config ===================== */
#define EMA_ALPHA_HEART  0.15f
#define EMA_ALPHA_BREATH 0.20f
#define EMA_ALPHA_POS    0.30f

static float s_filt_heart_rate  = 0.0f;
static float s_filt_breath_rate = 0.0f;
static float s_filt_pos_x       = 0.0f;
static float s_filt_pos_y       = 0.0f;
static float s_filt_pos_z       = 0.0f;
static bool  s_filt_heart_init  = false;
static bool  s_filt_breath_init = false;
static bool  s_filt_pos_init    = false;

/* ===================== Config ===================== */
#define LD6002_READ_BUF_SIZE      512
#define LD6002_READ_TIMEOUT_MS    200
#define LD6002_STATS_INTERVAL_MS  10000
#define LD6002_QUERY_INTERVAL_MS  10000
#define LD6002_HEADER_SIZE        8
#define LD6002_MAX_PAYLOAD        128
#define LD6002_MAX_FRAME_SIZE     (LD6002_HEADER_SIZE + LD6002_MAX_PAYLOAD + 1)
#define LD6002_SOF                0x01

#define LD6002_TASK_STACK_SIZE    4096
#define LD6002_TASK_PRIORITY      PRIORITY_RADAR
#define LD6002_TASK_NAME          "radar_ld6002"

/* ===================== TF Parser (matches standalone test) ===================== */
typedef struct {
    uint8_t  buf[LD6002_MAX_FRAME_SIZE];
    size_t   pos;
    size_t   expected_len;     /* total frame length */
    uint32_t valid_frames;
    uint32_t ck_errors;
    uint32_t sof_found;
    uint32_t hdr_ck_fails;
} tf_parser_t;

static tf_parser_t s_tfp = {0};

/* Checksum = ~(XOR over bytes) -- matches standalone tf_parser_calculate_checksum */
static uint8_t calc_ck(const uint8_t *data, size_t len)
{
    uint8_t x = 0;
    for (size_t i = 0; i < len; i++) x ^= data[i];
    return (uint8_t)(~x);
}

/* ===================== Latest Radar State ===================== */
/* Parser and filtered values belong to the one UART reader task. Public
 * getters use the paired snapshot below, never the live parser storage. */
static radar_ld6002_state_t s_radar = {0};
static radar_ld6002_state_t s_snapshot = {0};
static radar_ld6002_stats_t s_stats = {0};
static portMUX_TYPE s_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_control;
static SemaphoreHandle_t s_done;
static bool s_initialized;
static bool s_stopping;
static uint32_t s_bytes;
static int64_t s_last_valid_ms;

static float rd_float(const uint8_t *d, size_t off)
{
    float v;
    memcpy(&v, d + off, 4);
    return v;
}

/* ===================== Frame Decoder ===================== */
static void decode_frame(const uint8_t *buf, uint16_t msg_type, uint16_t data_len)
{
    const uint8_t *data = buf + LD6002_HEADER_SIZE;

    switch (msg_type) {
    case 0x0A13:  /* 相位测试 */
        if (data_len >= 12) {
            s_radar.total_phase  = rd_float(data, 0);
            s_radar.breath_phase = rd_float(data, 4);
            s_radar.heart_phase  = rd_float(data, 8);
            s_radar.has_phase    = true;
        }
        break;
    case 0x0A14:  /* 呼吸速率 */
        if (data_len >= 4) {
            s_radar.breath_rate = rd_float(data, 0);
            s_radar.has_breath  = true;
            if (!s_filt_breath_init) {
                s_filt_breath_rate = s_radar.breath_rate;
                s_filt_breath_init = true;
            } else {
                s_filt_breath_rate = EMA_ALPHA_BREATH * s_radar.breath_rate
                                   + (1.0f - EMA_ALPHA_BREATH) * s_filt_breath_rate;
            }

            radar_breath_data_t bd = {
                .is_detected  = true,
                .breath_phase = s_radar.breath_phase,
                .breath_rate  = s_filt_breath_rate,
            };
            event_t ev = {
                .type      = EV_RADAR_BREATH,
                .data      = &bd,
                .data_size = sizeof(bd),
                .timestamp = event_bus_get_timestamp(),
            };
            event_bus_publish(&ev);
        }
        break;
    case 0x0A15:  /* 心率 */
        if (data_len >= 4) {
            s_radar.heart_rate = rd_float(data, 0);
            s_radar.has_heart  = true;
            s_radar.heart_updated_ms = esp_timer_get_time() / 1000;
            if (!s_filt_heart_init) {
                s_filt_heart_rate = s_radar.heart_rate;
                s_filt_heart_init = true;
            } else {
                s_filt_heart_rate = EMA_ALPHA_HEART * s_radar.heart_rate
                                  + (1.0f - EMA_ALPHA_HEART) * s_filt_heart_rate;
            }

            radar_heart_data_t hd = {
                .is_valid    = true,
                .heart_rate  = s_filt_heart_rate,
                .heart_phase = s_radar.heart_phase,
            };
            event_t ev = {
                .type      = EV_RADAR_HEART_RATE,
                .data      = &hd,
                .data_size = sizeof(hd),
                .timestamp = event_bus_get_timestamp(),
            };
            event_bus_publish(&ev);
        }
        break;
    case 0x0F09:  /* 人体存在 */
        if (data_len < 1) break;
        s_radar.person_present = (data_len >= 1 && data[0] != 0);
        s_radar.has_presence   = true;
        s_radar.presence_updated_ms = esp_timer_get_time() / 1000;

        radar_presence_data_t pd = {
            .is_present  = s_radar.person_present,
            .distance_cm = s_radar.range_dist,
            .x           = s_filt_pos_x,
            .y           = s_filt_pos_y,
            .z           = s_filt_pos_z,
        };
        event_t ev = {
            .type      = EV_RADAR_PRESENCE,
            .data      = &pd,
            .data_size = sizeof(pd),
            .timestamp = event_bus_get_timestamp(),
        };
        event_bus_publish(&ev);
        break;
    case 0x0A04:  /* 人员位置 */
        if (data_len >= 4) {
            memcpy(&s_radar.target_count, data, 4);
            s_radar.has_pos = true;
        }
        break;
    case 0x0A16:  /* target range, original LD6002 reader */
    case 0x0A18:  /* target range, project_config protocol ID */
        if (data_len >= 8) {
            memcpy(&s_radar.range_flag, data, 4);
            s_radar.range_dist = rd_float(data, 4);
            s_radar.has_range  = true;

            radar_target_range_data_t rd = {
                .flag     = s_radar.range_flag,
                .range_cm = s_radar.range_dist,
            };
            event_t ev = {
                .type      = EV_RADAR_TARGET_RANGE,
                .data      = &rd,
                .data_size = sizeof(rd),
                .timestamp = event_bus_get_timestamp(),
            };
            event_bus_publish(&ev);
        }
        break;
    case 0x0A17:  /* 跟踪目标位置 */
        if (data_len >= 8) {
            s_radar.pos_x = rd_float(data, 0);
            s_radar.pos_y = rd_float(data, 4);
            if (data_len >= 12) s_radar.pos_z = rd_float(data, 8);
            else                s_radar.pos_z = 0.0f;
            if (!s_filt_pos_init) {
                s_filt_pos_x = s_radar.pos_x;
                s_filt_pos_y = s_radar.pos_y;
                s_filt_pos_z = s_radar.pos_z;
                s_filt_pos_init = true;
            } else {
                s_filt_pos_x = EMA_ALPHA_POS * s_radar.pos_x + (1.0f - EMA_ALPHA_POS) * s_filt_pos_x;
                s_filt_pos_y = EMA_ALPHA_POS * s_radar.pos_y + (1.0f - EMA_ALPHA_POS) * s_filt_pos_y;
                s_filt_pos_z = EMA_ALPHA_POS * s_radar.pos_z + (1.0f - EMA_ALPHA_POS) * s_filt_pos_z;
            }
        }
        break;
    default:
        break;
    }
}

/* ===================== Feed one byte (like tf_parser_input) ===================== */
static void feed_byte(tf_parser_t *tp, uint8_t byte)
{
    if (tp->pos == 0) {
        /* Wait for SOF */
        if (byte != LD6002_SOF) return;
        tp->sof_found++;
    }

    if (tp->pos >= sizeof(tp->buf)) {
        /* Overflow -- reset */
        tp->pos = 0;
        tp->expected_len = 0;
        return;
    }

    tp->buf[tp->pos++] = byte;

    /* After header is complete (8 bytes), compute expected frame length */
    if (tp->pos == LD6002_HEADER_SIZE) {
        uint16_t data_len = (uint16_t)((tp->buf[3] << 8) | tp->buf[4]);
        if (data_len > LD6002_MAX_PAYLOAD) {
            tp->pos = 0;  /* invalid length */
            return;
        }
        tp->expected_len = LD6002_HEADER_SIZE + data_len;
        if (data_len > 0) {
            tp->expected_len += 1;  /* +1 for data checksum */
        }

        if (tp->expected_len > sizeof(tp->buf)) {
            tp->pos = 0;
            tp->expected_len = 0;
            return;
        }
    }

    if (tp->expected_len == 0 || tp->pos < tp->expected_len) {
        return;  /* need more bytes */
    }

    /* ===== Full frame received, validate ===== */
    uint16_t dl = (uint16_t)((tp->buf[3] << 8) | tp->buf[4]);

    /* 1. Header checksum */
    if (tp->buf[7] != calc_ck(tp->buf, 7)) {
        tp->hdr_ck_fails++;
        tp->pos = 0;
        tp->expected_len = 0;
        return;
    }

    /* 2. Data checksum (only when data present) */
    if (dl > 0) {
        uint8_t dck = tp->buf[LD6002_HEADER_SIZE + dl];  /* last byte */
        if (dck != calc_ck(tp->buf + LD6002_HEADER_SIZE, dl)) {
            tp->ck_errors++;
            tp->pos = 0;
            tp->expected_len = 0;
            return;
        }
    }

    /* Valid frame! */
    tp->valid_frames++;
    s_last_valid_ms = esp_timer_get_time() / 1000;
    uint16_t msg_type = (uint16_t)((tp->buf[5] << 8) | tp->buf[6]);
    decode_frame(tp->buf, msg_type, dl);
    tp->pos = 0;
    tp->expected_len = 0;
}

/* ===================== Chinese Summary (每 10 秒打印一次状态) ===================== */
static void print_summary(const radar_ld6002_state_t *r, uint64_t elapsed_sec, uint32_t valid)
{
    ESP_LOGI(TAG, "Radar summary: t=%llus rx_bytes=%lu frames=%lu header_err=%lu data_err=%lu age_ms=%lld presence_valid=%d present=%d heart_valid=%d heart=%.1f breath_valid=%d breath=%.1f range_valid=%d range=%.1f",
             (unsigned long long)elapsed_sec, (unsigned long)s_bytes,
             (unsigned long)valid, (unsigned long)s_tfp.hdr_ck_fails,
             (unsigned long)s_tfp.ck_errors,
             s_last_valid_ms ? (long long)(esp_timer_get_time()/1000 - s_last_valid_ms) : -1LL,
             r->has_presence, r->person_present, r->has_heart,
             (double)s_filt_heart_rate, r->has_breath, (double)s_filt_breath_rate,
             r->has_range, (double)r->range_dist);
    if (!valid) ESP_LOGW(TAG, "UART configured, but no valid LD6002 frame yet; check radar power/TX/RX");
}

/* ===================== Firmware Query ===================== */
static void send_firmware_query(void)
{
    static const uint8_t cmd[] = {0x01, 0x00, 0x01, 0x00, 0x00, 0xFF, 0xFF, 0xFF};
    esp_err_t ret = radar_driver_send_command((uint8_t *)cmd, sizeof(cmd));
    if (ret != ESP_OK) ESP_LOGW(TAG, "Firmware query TX failed: %s", esp_err_to_name(ret));
}

/* ===================== Task ===================== */
static TaskHandle_t s_task_handle = NULL;
static volatile bool s_running = false;

static void radar_ld6002_task(void *arg)
{
    (void)arg;

    radar_driver_flush_input();
    send_firmware_query();

    uint8_t  buf[LD6002_READ_BUF_SIZE];
    int64_t  start_us        = esp_timer_get_time();
    int64_t  last_summary_ms = 0;
    int64_t  last_query_ms   = 0;

    ESP_LOGI(TAG, "task started");

    while (s_running) {
        int n = radar_driver_read_bytes(buf, sizeof(buf),
                                        pdMS_TO_TICKS(LD6002_READ_TIMEOUT_MS));
        if (n > 0) {
            s_bytes += n;
            for (int i = 0; i < n; i++)
                feed_byte(&s_tfp, buf[i]);
        }

        /* Drain UART events, handle overflow */
        QueueHandle_t event_queue = radar_driver_get_event_queue();
        if (event_queue) {
            uart_event_t evt;
            while (xQueueReceive(event_queue, &evt, 0) == pdPASS) {
                if (evt.type == UART_FIFO_OVF || evt.type == UART_BUFFER_FULL) {
                    s_tfp.pos = 0;
                    s_tfp.expected_len = 0;
                    radar_driver_flush_input();
                }
            }
        }

        portENTER_CRITICAL(&s_snapshot_lock);
        s_snapshot = s_radar;
        s_stats = (radar_ld6002_stats_t) {
            .valid_frames = s_tfp.valid_frames,
            .ck_errors = s_tfp.ck_errors,
            .sof_found = s_tfp.sof_found,
            .hdr_ck_fails = s_tfp.hdr_ck_fails,
            .rx_bytes = s_bytes,
            .last_valid_ms = s_last_valid_ms
        };
        portEXIT_CRITICAL(&s_snapshot_lock);
        /* No invented values before a complete checked radar frame. */
        if (s_radar.has_presence || s_radar.has_heart ||
            s_radar.has_breath || s_radar.has_range)
            device_state_set_radar(s_radar.person_present, s_filt_heart_rate,
                                   s_filt_breath_rate, s_radar.range_dist, 0, 0);
        int64_t now_ms = (esp_timer_get_time() - start_us) / 1000;

        /* 每 10 秒发送一次固件查询 */
        if (now_ms - last_query_ms >= LD6002_QUERY_INTERVAL_MS) {
            last_query_ms = now_ms;
            send_firmware_query();
        }

        /* 每 10 秒打印状态 */
        if (now_ms - last_summary_ms >= LD6002_STATS_INTERVAL_MS) {
            last_summary_ms = now_ms;
            print_summary(&s_radar, (uint64_t)(now_ms / 1000),
                          s_tfp.valid_frames);
        }
    }

    ESP_LOGI(TAG, "task exiting");
    /* Nothing touches UART/parser/state after the done signal. Dynamic
     * task stack reclamation remains owned by FreeRTOS, not this module. */
    portENTER_CRITICAL(&s_snapshot_lock);
    s_task_handle = NULL;
    portEXIT_CRITICAL(&s_snapshot_lock);
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

/* ===================== Public API ===================== */

esp_err_t radar_ld6002_init(void)
{
    /* Startup owner initializes once before any task/API consumers. */
    if (!s_control) s_control = xSemaphoreCreateMutex();
    if (!s_control) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_control, portMAX_DELAY);
    if (s_initialized) { xSemaphoreGive(s_control); return ESP_OK; }
    if (!s_done) s_done = xSemaphoreCreateBinary();
    if (!s_done) { xSemaphoreGive(s_control); return ESP_ERR_NO_MEM; }
    esp_err_t ret = radar_driver_init();
    if (ret == ESP_OK) {
        memset(&s_tfp, 0, sizeof(s_tfp));
        memset(&s_radar, 0, sizeof(s_radar));
        portENTER_CRITICAL(&s_snapshot_lock);
        memset(&s_snapshot, 0, sizeof(s_snapshot));
        memset(&s_stats, 0, sizeof(s_stats));
        portEXIT_CRITICAL(&s_snapshot_lock);
        s_filt_heart_init = s_filt_breath_init = s_filt_pos_init = false;
        s_filt_heart_rate = s_filt_breath_rate = 0;
        s_filt_pos_x = s_filt_pos_y = s_filt_pos_z = 0;
        s_bytes = 0;
        s_last_valid_ms = 0;
        s_initialized = true;
        ESP_LOGI(TAG, "LD6002 reader initialized (SOF=0x01, BE fields, XOR-NOT)");
    }
    xSemaphoreGive(s_control);
    return ret;
}

esp_err_t radar_ld6002_start(void)
{
    if (!s_control) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_control, portMAX_DELAY);
    if (!s_initialized || s_stopping) {
        xSemaphoreGive(s_control);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_running) { xSemaphoreGive(s_control); return ESP_OK; }
    xSemaphoreTake(s_done, 0);
    s_running = true;
    BaseType_t ret = xTaskCreate(radar_ld6002_task, LD6002_TASK_NAME,
                                LD6002_TASK_STACK_SIZE, NULL,
                                LD6002_TASK_PRIORITY, &s_task_handle);
    if (ret != pdPASS) {
        s_running = false;
        s_task_handle = NULL;
    }
    xSemaphoreGive(s_control);
    return ret == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

/* Caller holds s_control; worker needs no such lock to finish. */
static esp_err_t stop_locked(void)
{
    portENTER_CRITICAL(&s_snapshot_lock);
    bool active = s_task_handle != NULL;
    portEXIT_CRITICAL(&s_snapshot_lock);
    if (!active) { s_stopping = false; return ESP_OK; }
    s_stopping = true;
    s_running = false;
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(TAG, "Radar stop timeout; task/UART retained");
        return ESP_ERR_TIMEOUT;
    }
    s_stopping = false;
    return ESP_OK;
}

esp_err_t radar_ld6002_stop(void)
{
    if (!s_control) return ESP_OK;
    xSemaphoreTake(s_control, portMAX_DELAY);
    esp_err_t ret = stop_locked();
    xSemaphoreGive(s_control);
    return ret;
}

esp_err_t radar_ld6002_deinit(void)
{
    if (!s_control) return ESP_OK;
    xSemaphoreTake(s_control, portMAX_DELAY);
    esp_err_t ret = stop_locked();
    if (ret == ESP_OK) {
        ret = radar_driver_deinit();
        if (ret == ESP_OK) s_initialized = false;
    }
    xSemaphoreGive(s_control);
    return ret;
}

bool radar_ld6002_is_running(void)
{
    if (!s_control) return false;
    xSemaphoreTake(s_control, portMAX_DELAY);
    bool running = s_running;
    xSemaphoreGive(s_control);
    return running;
}

esp_err_t radar_ld6002_get_state(radar_ld6002_state_t *state)
{
    if (!state) return ESP_ERR_INVALID_ARG;
    if (!s_control) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_control, portMAX_DELAY);
    if (!s_initialized) { xSemaphoreGive(s_control); return ESP_ERR_INVALID_STATE; }
    portENTER_CRITICAL(&s_snapshot_lock);
    *state = s_snapshot;
    portEXIT_CRITICAL(&s_snapshot_lock);
    xSemaphoreGive(s_control);
    return ESP_OK;
}

esp_err_t radar_ld6002_get_stats(radar_ld6002_stats_t *stats)
{
    if (!stats) return ESP_ERR_INVALID_ARG;
    if (!s_control) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_control, portMAX_DELAY);
    if (!s_initialized) { xSemaphoreGive(s_control); return ESP_ERR_INVALID_STATE; }
    portENTER_CRITICAL(&s_snapshot_lock);
    *stats = s_stats;
    portEXIT_CRITICAL(&s_snapshot_lock);
    xSemaphoreGive(s_control);
    return ESP_OK;
}
