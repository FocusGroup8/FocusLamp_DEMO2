/*
 * radar_module.c - Radar module implementation
 *
 * Receives raw UART data from radar_driver, feeds it through a TF frame
 * parser state machine, and publishes parsed frames to a frame queue.
 */

#include "radar_module.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "radar_driver.h"
#include "system_config.h"

/*
 * Minimal TF frame parser embedded here to avoid a hard dependency on
 * the external tf_parser component.  The parser implements the same
 * frame format as the FocusLamp tf_parser:
 *
 *   [SOF=0x7E][frame_id:2][data_len:2][msg_type:2][hdr_csum:1][data:var][data_csum:1]
 */

/* ---- TF Frame Layout ---- */
#define TF_SOF                  0x7E
#define TF_HEADER_SIZE          RADAR_MODULE_TF_HEADER_SIZE
#define TF_MIN_FRAME_SIZE       9          /* header + 1 byte data + data_csum */
#define TF_MAX_PAYLOAD_SIZE     RADAR_MODULE_TF_MAX_PAYLOAD_SIZE
#define TF_MAX_FRAME_SIZE       RADAR_MODULE_TF_MAX_FRAME_SIZE

/* Parser states */
typedef enum {
    ST_IDLE,
    ST_FRAME_ID_L,
    ST_FRAME_ID_H,
    ST_DATA_LEN_L,
    ST_DATA_LEN_H,
    ST_MSG_TYPE_L,
    ST_MSG_TYPE_H,
    ST_HEADER_CSUM,
    ST_DATA,
    ST_DATA_CSUM,
} parser_state_t;

/* Parser context */
typedef struct {
    uint8_t   buffer[TF_MAX_FRAME_SIZE];
    size_t    position;
    size_t    expected_frame_length;
    parser_state_t state;
} tf_parser_t;

/* ---- End TF parser types ---- */

static const char *TAG = "radar_module";

#if (RADAR_MODULE_LOG_ENABLE == 1)
#define RADAR_LOG_ENABLED 1
#else
#define RADAR_LOG_ENABLED 0
#endif

#define RADAR_LOGI(fmt, ...)                          \
    do                                                \
    {                                                 \
        if (RADAR_LOG_ENABLED && s_radar_log_enabled) \
            ESP_LOGI(TAG, fmt, ##__VA_ARGS__);        \
    } while (0)
#define RADAR_LOGD(fmt, ...)                          \
    do                                                \
    {                                                 \
        if (RADAR_LOG_ENABLED && s_radar_log_enabled) \
            ESP_LOGD(TAG, fmt, ##__VA_ARGS__);        \
    } while (0)

/* ---- Static state ---- */
static tf_parser_t             s_tf_parser;
static radar_module_snapshot_t s_radar_snapshot           = {0};
static bool                    s_radar_module_initialized = false;
static bool                    s_radar_module_running     = false;
static QueueHandle_t           s_frame_queue              = NULL;
static bool                    s_radar_log_enabled        = false;

/* ===================== Parser Implementation ===================== */

static uint8_t tf_calc_checksum(const uint8_t *data, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += data[i];
    }
    return sum;
}

static void tf_parser_init_internal(tf_parser_t *parser)
{
    memset(parser, 0, sizeof(*parser));
    parser->state = ST_IDLE;
}

static void tf_parser_reset_internal(tf_parser_t *parser)
{
    memset(parser->buffer, 0, sizeof(parser->buffer));
    parser->position              = 0;
    parser->expected_frame_length = 0;
    parser->state                 = ST_IDLE;
}

static radar_parser_result_t tf_parser_input_internal(tf_parser_t *parser,
                                                       uint8_t      byte,
                                                       tf_frame_t  *frame)
{
    if (parser == NULL || frame == NULL) {
        return RADAR_PARSER_INVALID_ARG;
    }

    if (parser->position >= sizeof(parser->buffer)) {
        tf_parser_reset_internal(parser);
        return RADAR_PARSER_BUFFER_OVERFLOW;
    }

    parser->buffer[parser->position++] = byte;

    switch (parser->state) {
    case ST_IDLE:
        if (byte == TF_SOF) {
            parser->state = ST_FRAME_ID_L;
        } else {
            parser->position = 0;  /* discard */
        }
        break;

    case ST_FRAME_ID_L:
        parser->state = ST_FRAME_ID_H;
        break;
    case ST_FRAME_ID_H:
        parser->state = ST_DATA_LEN_L;
        break;

    case ST_DATA_LEN_L:
        parser->state = ST_DATA_LEN_H;
        break;
    case ST_DATA_LEN_H: {
        /* data_length is at buffer[3:4] */
        uint16_t data_len = (uint16_t)(parser->buffer[3] | ((uint16_t)parser->buffer[4] << 8));
        if (data_len > TF_MAX_PAYLOAD_SIZE) {
            tf_parser_reset_internal(parser);
            return RADAR_PARSER_INVALID_LENGTH;
        }
        parser->expected_frame_length = TF_HEADER_SIZE + data_len + 1; /* + data csum */
        if (parser->expected_frame_length > sizeof(parser->buffer)) {
            tf_parser_reset_internal(parser);
            return RADAR_PARSER_BUFFER_OVERFLOW;
        }
        parser->state = ST_MSG_TYPE_L;
        break;
    }

    case ST_MSG_TYPE_L:
        parser->state = ST_MSG_TYPE_H;
        break;
    case ST_MSG_TYPE_H:
        parser->state = ST_HEADER_CSUM;
        break;

    case ST_HEADER_CSUM: {
        /* Validate header checksum: SOF + frame_id:2 + data_len:2 + msg_type:2 = 7 bytes */
        uint8_t expected_csum = tf_calc_checksum(parser->buffer, 7);
        if (byte != expected_csum) {
            tf_parser_reset_internal(parser);
            return RADAR_PARSER_HEADER_CHECKSUM_ERROR;
        }
        if (parser->expected_frame_length == TF_HEADER_SIZE + 1) {
            /* No data payload, go straight to data checksum */
            parser->state = ST_DATA_CSUM;
        } else {
            parser->state = ST_DATA;
        }
        break;
    }

    case ST_DATA:
        if (parser->position >= parser->expected_frame_length - 1) {
            parser->state = ST_DATA_CSUM;
        }
        break;

    case ST_DATA_CSUM: {
        /* Validate data checksum */
        size_t data_start   = TF_HEADER_SIZE;
        size_t data_len     = parser->expected_frame_length - TF_HEADER_SIZE - 1;
        uint8_t expected_dc = tf_calc_checksum(parser->buffer + data_start, data_len);
        if (byte != expected_dc) {
            tf_parser_reset_internal(parser);
            return RADAR_PARSER_DATA_CHECKSUM_ERROR;
        }

        /* Frame complete - populate output */
        uint16_t data_length = (uint16_t)(parser->buffer[3] | ((uint16_t)parser->buffer[4] << 8));

        frame->frame_id        = (uint16_t)(parser->buffer[1] | ((uint16_t)parser->buffer[2] << 8));
        frame->data_length     = data_length;
        frame->message_type    = (uint16_t)(parser->buffer[5] | ((uint16_t)parser->buffer[6] << 8));
        frame->header_checksum = parser->buffer[7];
        frame->data_checksum   = byte;
        frame->total_length    = parser->expected_frame_length;

        if (data_length > 0) {
            memcpy(frame->data, parser->buffer + TF_HEADER_SIZE, data_length);
        }
        if (data_length < TF_MAX_PAYLOAD_SIZE) {
            memset(frame->data + data_length, 0, TF_MAX_PAYLOAD_SIZE - data_length);
        }

        tf_parser_reset_internal(parser);
        return RADAR_PARSER_OK;
    }

    default:
        tf_parser_reset_internal(parser);
        return RADAR_PARSER_INVALID_ARG;
    }

    return RADAR_PARSER_INCOMPLETE;
}

/* ===================== Module Internals ===================== */

static void radar_module_reset_snapshot(void)
{
    memset(&s_radar_snapshot, 0, sizeof(s_radar_snapshot));
    s_radar_snapshot.status             = RADAR_MODULE_STATUS_IDLE;
    s_radar_snapshot.last_parser_result = RADAR_PARSER_OK;
}

static void radar_module_mark_parser_error(radar_parser_result_t parser_result)
{
    s_radar_snapshot.status             = RADAR_MODULE_STATUS_ERROR;
    s_radar_snapshot.last_parser_result = parser_result;
    s_radar_snapshot.has_new_frame      = false;
    s_radar_snapshot.total_parser_errors++;
    s_radar_snapshot.consecutive_errors++;

    RADAR_LOGD("Parser error: result=%d, consecutive=%u, total=%u",
               (int)parser_result,
               (unsigned)s_radar_snapshot.consecutive_errors,
               (unsigned)s_radar_snapshot.total_parser_errors);
}

static esp_err_t radar_module_feed_bytes(const uint8_t *buffer, size_t length)
{
    if (buffer == NULL || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    s_radar_snapshot.last_receive_time_us = esp_timer_get_time();
    RADAR_LOGI("Processing %u bytes", (unsigned)length);

    for (size_t i = 0; i < length; ++i) {
        tf_frame_t         parsed_frame  = {0};
        radar_parser_result_t parser_result =
            tf_parser_input_internal(&s_tf_parser, buffer[i], &parsed_frame);

        s_radar_snapshot.total_bytes_processed++;

        if (parser_result == RADAR_PARSER_INCOMPLETE) {
            continue;
        }

        if (parser_result != RADAR_PARSER_OK) {
            radar_module_mark_parser_error(parser_result);
            ESP_LOGW(TAG, "Parser returned error %d", (int)parser_result);

            if (s_radar_snapshot.consecutive_errors >= RADAR_MODULE_MAX_CONSECUTIVE_ERRORS) {
                ESP_LOGW(TAG, "Too many consecutive errors, auto-resetting parser");
                tf_parser_reset_internal(&s_tf_parser);
                s_radar_snapshot.consecutive_errors = 0;
            }
            continue;
        }

        s_radar_snapshot.last_frame = parsed_frame;
        s_radar_snapshot.last_parser_result = parser_result;
        s_radar_snapshot.total_frames_parsed++;
        s_radar_snapshot.has_new_frame      = true;
        s_radar_snapshot.status             = RADAR_MODULE_STATUS_FRAME_READY;
        s_radar_snapshot.consecutive_errors = 0;

        RADAR_LOGI("Frame parsed: ID=0x%04X, Type=0x%04X, Len=%u",
                   parsed_frame.frame_id, parsed_frame.message_type,
                   parsed_frame.data_length);

        if (s_frame_queue != NULL) {
            BaseType_t qr = xQueueSend(s_frame_queue, &parsed_frame, 0);
            if (qr != pdPASS) {
                ESP_LOGW(TAG, "Frame queue full, dropping frame ID=0x%04X",
                         parsed_frame.frame_id);
            } else {
                RADAR_LOGI("Frame queued: ID=0x%04X, Type=0x%04X",
                           parsed_frame.frame_id, parsed_frame.message_type);
            }
        }
    }

    if (!s_radar_snapshot.has_new_frame && s_radar_snapshot.status != RADAR_MODULE_STATUS_ERROR) {
        s_radar_snapshot.status = RADAR_MODULE_STATUS_RUNNING;
    }

    return ESP_OK;
}

/* ===================== Public API ===================== */

esp_err_t radar_module_init(void)
{
    if (s_radar_module_initialized) {
        ESP_LOGW(TAG, "Radar module already initialized");
        return ESP_OK;
    }

    RADAR_LOGI("Initializing radar module...");

    radar_module_reset_snapshot();
    tf_parser_init_internal(&s_tf_parser);

    s_frame_queue = xQueueCreate(RADAR_MODULE_FRAME_QUEUE_SIZE, sizeof(tf_frame_t));
    if (s_frame_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create frame queue");
        return ESP_ERR_NO_MEM;
    }

    RADAR_LOGI("Frame queue created: size=%u", RADAR_MODULE_FRAME_QUEUE_SIZE);

    esp_err_t ret = radar_driver_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "radar_driver_init failed: %s", esp_err_to_name(ret));
        vQueueDelete(s_frame_queue);
        s_frame_queue = NULL;
        return ret;
    }

    s_radar_snapshot.status    = RADAR_MODULE_STATUS_RUNNING;
    s_radar_module_initialized = true;
    s_radar_module_running     = true;
    s_radar_log_enabled        = false;

    ESP_LOGI(TAG, "Radar module initialized");
    return ESP_OK;
}

esp_err_t radar_module_deinit(void)
{
    if (!s_radar_module_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = radar_driver_deinit();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "radar_driver_deinit failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (s_frame_queue != NULL) {
        vQueueDelete(s_frame_queue);
        s_frame_queue = NULL;
        RADAR_LOGI("Frame queue deleted");
    }

    tf_parser_reset_internal(&s_tf_parser);
    radar_module_reset_snapshot();
    s_radar_module_initialized = false;
    s_radar_module_running     = false;
    ESP_LOGI(TAG, "Radar module deinitialized");
    return ESP_OK;
}

esp_err_t radar_module_start(void)
{
    if (!s_radar_module_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_radar_module_running) {
        return ESP_OK;
    }
    s_radar_module_running  = true;
    s_radar_snapshot.status = RADAR_MODULE_STATUS_RUNNING;
    ESP_LOGI(TAG, "Radar module started");
    return ESP_OK;
}

esp_err_t radar_module_stop(void)
{
    if (!s_radar_module_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_radar_module_running) {
        return ESP_OK;
    }
    s_radar_module_running  = false;
    s_radar_snapshot.status = RADAR_MODULE_STATUS_IDLE;
    ESP_LOGI(TAG, "Radar module stopped");
    return ESP_OK;
}

bool radar_module_is_running(void)
{
    return s_radar_module_running;
}

QueueHandle_t radar_module_get_event_queue(void)
{
    if (!s_radar_module_initialized) {
        return NULL;
    }
    return radar_driver_get_event_queue();
}

QueueHandle_t radar_module_get_frame_queue(void)
{
    if (!s_radar_module_initialized) {
        return NULL;
    }
    return s_frame_queue;
}

esp_err_t radar_module_handle_uart_event(const uart_event_t *event)
{
    if (!s_radar_module_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (event->type) {
    case UART_DATA:
        return radar_module_poll(event->size, 0);

    case UART_FIFO_OVF:
    case UART_BUFFER_FULL:
        ESP_LOGW(TAG, "UART RX overflow detected, flushing input");
        tf_parser_reset_internal(&s_tf_parser);
        s_radar_snapshot.status             = RADAR_MODULE_STATUS_ERROR;
        s_radar_snapshot.last_parser_result = RADAR_PARSER_BUFFER_OVERFLOW;
        return radar_driver_flush_input();

    case UART_PARITY_ERR:
    case UART_FRAME_ERR:
        ESP_LOGW(TAG, "UART line error detected, type=%d", event->type);
        s_radar_snapshot.status             = RADAR_MODULE_STATUS_ERROR;
        s_radar_snapshot.last_parser_result = RADAR_PARSER_INVALID_LENGTH;
        return ESP_OK;

    default:
        return ESP_OK;
    }
}

esp_err_t radar_module_poll(size_t max_bytes_to_process, TickType_t ticks_to_wait)
{
    if (!s_radar_module_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_radar_module_running) {
        return ESP_OK;
    }
    if (max_bytes_to_process == 0 || max_bytes_to_process > RADAR_MODULE_UART_BUFFER_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t   buffer[RADAR_MODULE_UART_BUFFER_SIZE];
    const int bytes_read = radar_driver_read_bytes(buffer, max_bytes_to_process, ticks_to_wait);
    if (bytes_read < 0) {
        return ESP_FAIL;
    }
    if (bytes_read == 0) {
        return ESP_OK;
    }

    return radar_module_feed_bytes(buffer, (size_t)bytes_read);
}

esp_err_t radar_module_get_snapshot(radar_module_snapshot_t *snapshot)
{
    if (!s_radar_module_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *snapshot = s_radar_snapshot;
    return ESP_OK;
}

esp_err_t radar_module_clear_new_frame_flag(void)
{
    if (!s_radar_module_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    s_radar_snapshot.has_new_frame = false;
    if (s_radar_snapshot.status == RADAR_MODULE_STATUS_FRAME_READY) {
        s_radar_snapshot.status = RADAR_MODULE_STATUS_RUNNING;
    }
    return ESP_OK;
}

bool radar_module_is_initialized(void)
{
    return s_radar_module_initialized;
}

esp_err_t radar_module_reset_parser(void)
{
    if (!s_radar_module_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    tf_parser_reset_internal(&s_tf_parser);
    s_radar_snapshot.status             = RADAR_MODULE_STATUS_RUNNING;
    s_radar_snapshot.last_parser_result = RADAR_PARSER_OK;
    s_radar_snapshot.has_new_frame      = false;
    s_radar_snapshot.consecutive_errors = 0;
    ESP_LOGI(TAG, "Parser reset completed");
    return ESP_OK;
}

bool radar_module_is_in_error_state(void)
{
    return s_radar_snapshot.status == RADAR_MODULE_STATUS_ERROR;
}

bool radar_module_is_timeout(void)
{
    if (!s_radar_module_initialized) {
        return true;
    }
    if (s_radar_snapshot.last_receive_time_us == 0) {
        return true;
    }
    return (radar_module_get_time_since_last_receive_ms() > RADAR_MODULE_DATA_TIMEOUT_MS);
}

int64_t radar_module_get_time_since_last_receive_ms(void)
{
    if (!s_radar_module_initialized || s_radar_snapshot.last_receive_time_us == 0) {
        return -1;
    }
    return (esp_timer_get_time() - s_radar_snapshot.last_receive_time_us) / 1000;
}

esp_err_t radar_module_get_resource_status(char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int offset = 0;
    int written;

    written = snprintf(buffer + offset, buffer_size - offset,
                       "=== Radar Module Status ===\n");
    offset += written;
    written = snprintf(buffer + offset, buffer_size - offset,
                       "Initialized:       %s\n", s_radar_module_initialized ? "Yes" : "No");
    offset += written;
    written = snprintf(buffer + offset, buffer_size - offset,
                       "Status:            %d\n", (int)s_radar_snapshot.status);
    offset += written;
    written = snprintf(buffer + offset, buffer_size - offset,
                       "Total bytes:       %u\n", (unsigned)s_radar_snapshot.total_bytes_processed);
    offset += written;
    written = snprintf(buffer + offset, buffer_size - offset,
                       "Total frames:      %u\n", (unsigned)s_radar_snapshot.total_frames_parsed);
    offset += written;
    written = snprintf(buffer + offset, buffer_size - offset,
                       "Total errors:      %u\n", (unsigned)s_radar_snapshot.total_parser_errors);
    offset += written;
    written = snprintf(buffer + offset, buffer_size - offset,
                       "Consecutive errs:  %u\n", (unsigned)s_radar_snapshot.consecutive_errors);
    offset += written;

    int64_t t = radar_module_get_time_since_last_receive_ms();
    written   = snprintf(buffer + offset, buffer_size - offset,
                         "Last receive:      %lld ms ago\n", t);
    offset += written;
    written = snprintf(buffer + offset, buffer_size - offset,
                       "Timeout:           %s\n", radar_module_is_timeout() ? "Yes" : "No");
    offset += written;
    written = snprintf(buffer + offset, buffer_size - offset,
                       "Error state:       %s\n", radar_module_is_in_error_state() ? "Yes" : "No");
    offset += written;

    return ESP_OK;
}

void radar_module_enable_log(void)
{
#if (RADAR_LOG_ENABLED == 1)
    s_radar_log_enabled = true;
    ESP_LOGI(TAG, "Radar module log enabled");
#else
    ESP_LOGW(TAG, "Radar module log is disabled in config");
#endif
}

void radar_module_disable_log(void)
{
#if (RADAR_LOG_ENABLED == 1)
    s_radar_log_enabled = false;
    ESP_LOGI(TAG, "Radar module log disabled");
#else
    ESP_LOGW(TAG, "Radar module log is disabled in config");
#endif
}

bool radar_module_is_log_enabled(void)
{
    return s_radar_log_enabled;
}
