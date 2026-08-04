/*
 * radar_driver.c - Radar sensor driver implementation
 */

#include "radar_driver.h"
#include "project_config.h"
#include "system_config.h"

#include <string.h>

#include "esp_log.h"
#include "esp_err.h"

#include "driver/uart.h"

static const char *TAG = "radar_drv";

/* Radar UART comes from project_config.h to avoid conflicting with console UART0 */
#define RADAR_RX_BUF_SIZE       RADAR_UART_RX_BUFFER_SIZE
#define RADAR_TX_BUF_SIZE       RADAR_UART_TX_BUFFER_SIZE
#define RADAR_EVENT_QUEUE_LEN   RADAR_UART_EVENT_QUEUE_LEN

static bool          s_initialized        = false;
static QueueHandle_t s_event_queue        = NULL;

/* ===================== UART Driver ===================== */

esp_err_t radar_driver_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Radar driver already initialized");
        return ESP_OK;
    }

#if !RADAR_DRIVER_ENABLE
    ESP_LOGW(TAG, "Radar driver disabled (RADAR_DRIVER_ENABLE=0), skipping init");
    return ESP_ERR_NOT_SUPPORTED;
#endif

    const uart_config_t uart_config = {
        .baud_rate  = RADAR_UART_BAUD_RATE,
        .data_bits  = RADAR_UART_DATA_BITS,
        .parity     = RADAR_UART_PARITY,
        .stop_bits  = RADAR_UART_STOP_BITS,
        .flow_ctrl  = RADAR_UART_FLOW_CTRL,
        .source_clk = RADAR_UART_SOURCE_CLK,
    };

    /* ESP-IDF v5.x recommended order: param_config -> set_pin -> install */
    esp_err_t ret = uart_param_config(RADAR_UART_PORT, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_set_pin(RADAR_UART_PORT, RADAR_UART_TX_GPIO, RADAR_UART_RX_GPIO,
                       RADAR_UART_RTS_GPIO, RADAR_UART_CTS_GPIO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_driver_install(RADAR_UART_PORT, RADAR_RX_BUF_SIZE,
                              RADAR_TX_BUF_SIZE, RADAR_EVENT_QUEUE_LEN,
                              &s_event_queue, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_flush_input(RADAR_UART_PORT);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "uart_flush_input failed: %s", esp_err_to_name(ret));
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Radar UART initialized (port=%d, baud=%d, tx=%d, rx=%d)",
             RADAR_UART_PORT, RADAR_UART_BAUD_RATE, RADAR_UART_TX_GPIO, RADAR_UART_RX_GPIO);
    return ESP_OK;
}

esp_err_t radar_driver_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = uart_driver_delete(RADAR_UART_PORT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_delete failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_event_queue = NULL;
    s_initialized = false;
    ESP_LOGI(TAG, "Radar UART driver deinitialized");
    return ESP_OK;
}

QueueHandle_t radar_driver_get_event_queue(void)
{
    return s_event_queue;
}

int radar_driver_read_bytes(uint8_t *buffer, size_t length, TickType_t ticks_to_wait)
{
    if (!s_initialized || buffer == NULL || length == 0) {
        return -1;
    }
    return uart_read_bytes(RADAR_UART_PORT, buffer, length, ticks_to_wait);
}

esp_err_t radar_driver_flush_input(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return uart_flush_input(RADAR_UART_PORT);
}

esp_err_t radar_driver_send_command(uint8_t *cmd, uint16_t len)
{
    if (cmd == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    int ret = uart_write_bytes(RADAR_UART_PORT, (const char *)cmd, len);
    if (ret < 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

int radar_driver_read_data(uint8_t *buffer, uint16_t max_len)
{
    if (buffer == NULL || max_len == 0 || !s_initialized) {
        return -1;
    }
    return uart_read_bytes(RADAR_UART_PORT, buffer, max_len, pdMS_TO_TICKS(100));
}

esp_err_t radar_driver_parse_data(uint8_t *raw, radar_data_t *output)
{
    if (raw == NULL || output == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Placeholder: delegate to application-level parser */
    output->heart_rate       = 0.0f;
    output->respiration_rate = 0.0f;
    output->distance         = 0.0f;
    output->target_detected  = false;
    output->signal_quality   = 0;

    return ESP_OK;
}

/* ===================== TF Frame Decode Utilities ===================== */

/*
 * These functions assume the caller has included tf_parser.h before this header,
 * so that tf_frame_t is a complete type. The 'void *' parameter is cast internally.
 *
 * tf_frame_t layout (from tf_parser.h):
 *   uint16_t frame_id;
 *   uint16_t data_length;
 *   uint16_t message_type;
 *   uint8_t  header_checksum;
 *   uint8_t  data[TF_PARSER_MAX_PAYLOAD_SIZE];
 *   uint8_t  data_checksum;
 *   size_t   total_length;
 */

/* Internal helper: given a void* frame, extract data_length by casting offset */
/* We avoid including tf_parser.h here so this driver has no hard dependency on it. */

#define TF_FRAME_DATA_LENGTH_OFFSET  offsetof(struct { uint16_t frame_id; uint16_t data_length; }, data_length)
#define TF_FRAME_DATA_OFFSET         (sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint8_t))

static inline uint16_t frame_data_length(const void *frame)
{
    if (frame == NULL) return 0;
    /* data_length is at offset 2 (after frame_id) */
    const uint8_t *b = (const uint8_t *)frame;
    return (uint16_t)(b[2] | ((uint16_t)b[3] << 8));
}

static inline const uint8_t *frame_data_ptr(const void *frame)
{
    if (frame == NULL) return NULL;
    return (const uint8_t *)frame + TF_FRAME_DATA_OFFSET;
}

bool radar_driver_decode_bool(const void *frame)
{
    const uint16_t len = frame_data_length(frame);
    const uint8_t *data = frame_data_ptr(frame);
    return (data != NULL && len > 0 && data[0] != 0);
}

float radar_driver_decode_float_at(const void *frame, size_t offset)
{
    const uint16_t len = frame_data_length(frame);
    const uint8_t *data = frame_data_ptr(frame);
    if (data == NULL || offset + sizeof(float) > len) {
        return 0.0f;
    }
    float result;
    memcpy(&result, data + offset, sizeof(float));
    return result;
}

uint32_t radar_driver_decode_uint32_at(const void *frame, size_t offset)
{
    const uint16_t len = frame_data_length(frame);
    const uint8_t *data = frame_data_ptr(frame);
    if (data == NULL || offset + sizeof(uint32_t) > len) {
        return 0;
    }
    uint32_t result;
    memcpy(&result, data + offset, sizeof(uint32_t));
    return result;
}

int32_t radar_driver_decode_int32_at(const void *frame, size_t offset)
{
    const uint16_t len = frame_data_length(frame);
    const uint8_t *data = frame_data_ptr(frame);
    if (data == NULL || offset + sizeof(int32_t) > len) {
        return 0;
    }
    int32_t result;
    memcpy(&result, data + offset, sizeof(int32_t));
    return result;
}
