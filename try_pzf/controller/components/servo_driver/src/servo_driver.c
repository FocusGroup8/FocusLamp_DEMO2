/*
 * servo_driver.c - Serial bus servo driver implementation
 * Supports EM3 (0xFF header) and LX (0x55 header) bus servo protocols
 * via UART with OE pin half-duplex control.
 *
 * Contains both:
 *   - New index-based API (servo_driver_*)
 *   - Legacy bus-ID-based API (servo_em3_*, servo_lx_*)
 */

#include "servo_driver.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#define SERVO_OE1_GPIO GPIO_NUM_11
#define SERVO_OE2_GPIO GPIO_NUM_4
#include "system_config.h"

#include <string.h>
#include <stdio.h>
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/* ── Per-type UART and OE mapping ── */
static uart_port_t servo_uart(servo_type_t type)
{
    switch (type) {
        case SERVO_TYPE_EM3: return UART_NUM_1;
        case SERVO_TYPE_LX:  return UART_NUM_2;
        default:             return UART_NUM_1;
    }
}

static gpio_num_t servo_oe_pin(servo_type_t type)
{
    switch (type) {
        case SERVO_TYPE_EM3: return SERVO_OE1_GPIO;  /* GPIO11 */
        case SERVO_TYPE_LX:  return SERVO_OE2_GPIO;  /* GPIO4  */
        default:             return SERVO_OE1_GPIO;
    }
}

/* Protocol type per servo channel (0-based index) */
static const servo_type_t s_servo_types[SERVO_COUNT] = {
    SERVO_TYPE_EM3,  /* Servo 0: head up/down */
    SERVO_TYPE_LX,   /* Servo 1: arm/other */
};

/* Bus ID for each servo channel.
 * Servo 0 (EM3): bus ID = 4
 * Servo 1 (LX):  bus ID = 1 */
static const uint8_t s_servo_bus_ids[SERVO_COUNT] = {
    4,  /* Servo 0 (EM3) */
    1,  /* Servo 1 (LX)  */
};

#define SERVO_BUS_ID(id)        s_servo_bus_ids[(id)]

static bool s_servo_enabled[SERVO_COUNT] = { false };

/* UART mutexes for half-duplex bus arbitration */
static SemaphoreHandle_t s_uart_mutex[UART_NUM_2 + 1] = { NULL };
static bool s_driver_initialized = false;
static bool s_owned_uart[3];
static bool s_em3_initialized = false;
static bool s_lx_initialized = false;

static bool lock_servo_bus(uart_port_t uart)
{
    if (uart < 0 || uart > UART_NUM_2) {
        return false;
    }

    SemaphoreHandle_t mutex = s_uart_mutex[uart];
    if (mutex == NULL) {
        return false;
    }

    return xSemaphoreTake(mutex, pdMS_TO_TICKS(500)) == pdTRUE;
}

static void unlock_servo_bus(uart_port_t uart)
{
    if (uart < 0 || uart > UART_NUM_2) {
        return;
    }

    SemaphoreHandle_t mutex = s_uart_mutex[uart];
    if (mutex != NULL) {
        xSemaphoreGive(mutex);
    }
}

static uint8_t calc_checksum(const uint8_t *data, int len);

/* Query diagnosis is kept per bus and read by its sole executor. No safety decision
 * is made from cached positions. One failed query reports first/final attempts only. */
static esp_err_t s_query_tx_result[3];
static const char *s_query_stage[3];
static int s_last_written[3];

static const char *reply_reason(const uint8_t *rx, int n, const uint8_t *tx, int tx_len, bool lx)
{
    if (n<0) return "transport_error";
    if (n==0) return "no_rx_bytes";
    for (int i=0; i+tx_len<=n; ++i)
        if (!memcmp(rx+i,tx,tx_len)) return "query_echo";
    if (n<8) return "short_frame";
    const uint8_t header=lx ? 0x55 : 0xff;
    bool saw_header=false;
    for (int i=0; i+4<n; ++i) {
        if (rx[i]!=header || rx[i+1]!=header) continue;
        saw_header=true;
        if (rx[i+2]!=tx[2]) continue;
        int size=rx[i+3]+(lx ? 3 : 4);
        if (size<8) return "invalid_length";
        if (i+size>n) return "incomplete_frame";
        if (calc_checksum(rx+i,size-1)!=rx[i+size-1]) return "checksum_mismatch";
        if (lx && rx[i+4]!=0x1c) return "unexpected_command";
        if (!lx && rx[i+4]!=0) return "servo_status_error";
        int pos=rx[i+5] | (rx[i+6]<<8);
        return pos>(lx ? 1000 : 3000) ? "position_out_of_range" : "valid_frame";
    }
    return saw_header ? "unexpected_id" : "missing_header";
}

static void log_query(uart_port_t port, const uint8_t *tx, int tx_len,
                      const uint8_t *rx, int n, int attempt, bool lx)
{
    char raw[97]={0}, request[25]={0};
    for (int i=0; i<n && i<32; ++i) snprintf(raw+i*3,sizeof(raw)-i*3,"%02X ",rx[i]);
    for (int i=0; i<tx_len && i<8; ++i) snprintf(request+i*3,sizeof(request)-i*3,"%02X ",tx[i]);
    ESP_LOGW("servo_diag",
        "QUERY uart=%d id=%u attempt=%d stage=%s tx_status=%s written=%d/%d rx_bytes=%d reason=%s TX=[%s] RX=[%s]",
        port,(unsigned)tx[2],attempt,s_query_stage[port],
        esp_err_to_name(s_query_tx_result[port]),s_last_written[port],tx_len,n,
        reply_reason(rx,n,tx,tx_len,lx),request,raw);
}

static esp_err_t local_uart_write(uart_port_t port, const void *data, size_t len)
{
    if (!s_driver_initialized) return ESP_ERR_INVALID_STATE;
    int n = uart_write_bytes(port, data, len);
    s_last_written[port]=n;
    if (n != (int)len) return ESP_FAIL;
    return uart_wait_tx_done(port, pdMS_TO_TICKS(500));
}

/* ── Checksum: ~sum(bytes[2] ~ bytes[n-1]) ── */
static uint8_t calc_checksum(const uint8_t *data, int len)
{
    uint8_t sum = 0;
    for (int i = 2; i < len; i++) {
        sum += data[i];
    }
    return ~sum;
}

static esp_err_t servo_bus_send(uart_port_t uart, gpio_num_t oe, const uint8_t *data, uint16_t len)
{
    if (!data || !len || uart<1 || uart>2 ||
        oe!=(uart==1 ? SERVO_OE1_GPIO : SERVO_OE2_GPIO)) return ESP_ERR_INVALID_ARG;
    if (!lock_servo_bus(uart)) return ESP_ERR_TIMEOUT;
    esp_err_t ret=uart_flush_input(uart);
    /* Standard RTS asserts physical OE high at TX start, then TX_DONE ISR
     * releases it low BEFORE waking this task. No task-level GPIO toggling. */
    if (ret==ESP_OK) ret=local_uart_write(uart,data,len);
    /* base ran at 1000Hz; controller runs at 100Hz. Preserve the original
     * 3ms command-processing gap without truncating it to 0 ticks. OE is already
     * LOW here, so this gap cannot keep the receiver disabled during an ACK. */
    if (ret==ESP_OK) esp_rom_delay_us(3000);
    unlock_servo_bus(uart);
    return ret;
}

static int servo_bus_transaction(uart_port_t uart, gpio_num_t oe, const uint8_t *tx_data, uint16_t tx_len,
                                 uint8_t *rx_buf, uint16_t rx_len, uint16_t settle_ms, uint16_t timeout_ms)
{
    if (!tx_data || !rx_buf || uart<1 || uart>2 ||
        oe!=(uart==1 ? SERVO_OE1_GPIO : SERVO_OE2_GPIO)) return -1;
    s_query_tx_result[uart]=ESP_ERR_TIMEOUT;
    s_query_stage[uart]="bus_lock";
    s_last_written[uart]=0;
    if (!lock_servo_bus(uart)) return -1;
    s_query_stage[uart]="flush";
    esp_err_t ret=uart_flush_input(uart);
    if (ret==ESP_OK) {
        s_query_stage[uart]="uart_tx_wait_auto_oe";
        ret=local_uart_write(uart,tx_data,tx_len);
    }
    s_query_tx_result[uart]=ret;
    if (ret!=ESP_OK) { unlock_servo_bus(uart); return -1; }
    s_query_stage[uart]="uart_rx_auto_oe";
    /* RX is already enabled by TX_DONE ISR. Blocking receive collects the reply;
     * it does not require a pre-read sleep, especially one truncated to 0 ticks. */
    (void)settle_ms;
    TickType_t timeout=pdMS_TO_TICKS(timeout_ms);
    int n=uart_read_bytes(uart,rx_buf,rx_len,timeout>0 ? timeout : 1);
    unlock_servo_bus(uart);
    return n;
}

static bool parse_em3_position_response(const uint8_t *rx_buf, int len, uint8_t bus_id, uint16_t *position)
{
    if (rx_buf == NULL || position == NULL || len < 8) {
        return false;
    }

    for (int i = 0; i <= len - 8; i++) {
        if (rx_buf[i] == 0xFF && rx_buf[i + 1] == 0xFF && rx_buf[i + 2] == bus_id) {
            int frame_len = rx_buf[i + 3] + 4;
            if (frame_len < 8 || i + frame_len > len || rx_buf[i+4] != 0 ||
                calc_checksum(rx_buf+i, frame_len-1) != rx_buf[i+frame_len-1]) continue;
            int16_t pos = (int16_t)(rx_buf[i + 5] | (rx_buf[i + 6] << 8));
            if (pos >= 0 && pos <= 3000) {
                *position = (uint16_t)pos;
                return true;
            }
        }
    }

    return false;
}

static bool parse_lx_position_response(const uint8_t *rx_buf, int len, uint8_t bus_id, uint16_t *position)
{
    if (rx_buf == NULL || position == NULL || len < 8) {
        return false;
    }

    for (int i = 0; i <= len - 8; i++) {
        if (rx_buf[i] == 0x55 && rx_buf[i + 1] == 0x55 &&
            rx_buf[i + 2] == bus_id && rx_buf[i + 4] == 0x1C) {
            int frame_len = rx_buf[i + 3] + 3;
            if (frame_len < 8 || i + frame_len > len ||
                calc_checksum(rx_buf+i, frame_len-1) != rx_buf[i+frame_len-1]) continue;
            int16_t pos = (int16_t)(rx_buf[i + 5] | (rx_buf[i + 6] << 8));
            if (pos >= 0 && pos <= 1000) {
                *position = (uint16_t)pos;
                return true;
            }
        }
    }

    return false;
}

/* ── Assert OE, send on the servo's UART, release OE ── */
static esp_err_t send_with_oe(uint8_t servo_id, const uint8_t *data, uint16_t len)
{
    if (servo_id >= SERVO_COUNT) return ESP_ERR_INVALID_ARG;

    servo_type_t type = s_servo_types[servo_id];
    uart_port_t uart = servo_uart(type);
    gpio_num_t oe = servo_oe_pin(type);

    return servo_bus_send(uart, oe, data, len);
}

/* ── Assert OE, send, wait, release OE, read response ── */
static int send_then_receive(uint8_t servo_id, const uint8_t *tx_data, uint16_t tx_len,
                             uint8_t *rx_buf, uint16_t rx_len, uint16_t timeout_ms)
{
    if (servo_id >= SERVO_COUNT || tx_data == NULL || rx_buf == NULL) return -1;

    servo_type_t type = s_servo_types[servo_id];
    gpio_num_t oe = servo_oe_pin(type);
    uart_port_t uart = servo_uart(type);

    return servo_bus_transaction(uart, oe, tx_data, tx_len, rx_buf, rx_len, 5, timeout_ms);
}

/* ══════════════════════════════════════════════════════════════
   Public API (new index-based)
   ══════════════════════════════════════════════════════════════ */

esp_err_t servo_driver_init(void)
{
    if (s_driver_initialized) return ESP_OK;
    const int tx[3] = {0, 8, 3}, rx[3] = {0, 2, 20}, oe[3] = {0, 11, 4};
    gpio_config_t io = {.pin_bit_mask = (1ULL << 11) | (1ULL << 4),
                        .mode = GPIO_MODE_OUTPUT};
    esp_err_t ret = gpio_config(&io);
    if (ret != ESP_OK) return ret;
    gpio_set_level(GPIO_NUM_11, 0);
    gpio_set_level(GPIO_NUM_4, 0);
    for (int port = 1; port <= 2; ++port) {
        if (uart_is_driver_installed(port)) { ret = ESP_ERR_INVALID_STATE; goto fail; }
        s_uart_mutex[port] = xSemaphoreCreateMutex();
        if (!s_uart_mutex[port]) { ret = ESP_ERR_NO_MEM; goto fail; }
        uart_config_t cfg = {.baud_rate=115200, .data_bits=UART_DATA_8_BITS,
            .parity=UART_PARITY_DISABLE, .stop_bits=UART_STOP_BITS_1,
            .flow_ctrl=UART_HW_FLOWCTRL_DISABLE, .source_clk=UART_SCLK_DEFAULT};
        ret = uart_param_config(port, &cfg);
        if (ret != ESP_OK) goto fail;
        ret = uart_set_pin(port, tx[port], rx[port], UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        if (ret != ESP_OK) goto fail;
        ret = uart_driver_install(port, 2048, 2048, 0, NULL, 0);
        if (ret != ESP_OK) goto fail;
        s_owned_uart[port] = true;
        /* uart_set_rts(level=0) outputs HIGH; level=1 outputs LOW (uart.h).
         * Half-duplex uses 0 for TX and 1 for RX, exactly our active-high OE.
         * Do NOT invert RTS; its API parameter is not the physical voltage. */
        ret = uart_set_line_inverse(port, 0);
        if (ret != ESP_OK) goto fail;
        ret = uart_set_rts(port, 1); /* standard RTS: idle physical OE=0 */
        if (ret != ESP_OK) goto fail;
        ret = uart_set_pin(port, tx[port], rx[port], oe[port], UART_PIN_NO_CHANGE);
        if (ret != ESP_OK) goto fail;
        ret = uart_set_mode(port, UART_MODE_RS485_HALF_DUPLEX);
        if (ret != ESP_OK) goto fail;
    }
    s_driver_initialized = true;
    ESP_LOGI("servo_local", "UART1 TX8/RX2/OE11; UART2 TX3/RX20/OE4 ready");
    ESP_LOGI("servo_diag", "Half-duplex auto-OE enabled: TX_DONE ISR, standard RTS, GPIO11/4, TX=1 RX=0");
    ESP_LOGI("servo_diag", "Query diagnostics enabled: raw TX/RX, transport stage and parser reason");
    return ESP_OK;
fail:
    for (int port=1; port<=2; ++port) {
        if (s_owned_uart[port]) {
            uart_driver_delete(port); s_owned_uart[port]=false;
            gpio_reset_pin(oe[port]); gpio_set_direction(oe[port],GPIO_MODE_OUTPUT);
            gpio_set_level(oe[port],0);
        }
        if (s_uart_mutex[port]) { vSemaphoreDelete(s_uart_mutex[port]); s_uart_mutex[port]=NULL; }
    }
    return ret;
}

esp_err_t servo_driver_send_command(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0) return ESP_ERR_INVALID_ARG;
    /* Default to EM3 UART for raw command */
    return local_uart_write(UART_NUM_1, data, len);
}

int servo_driver_read_response(uint8_t *buffer, uint16_t timeout)
{
    if (buffer == NULL) return -1;
    /* Default to EM3 UART for response */
    return uart_read_bytes(UART_NUM_1, buffer, 16, pdMS_TO_TICKS(timeout));
}

/* ══════════════════════════════════════════════════════════════
   EM3 Protocol — set_position (writes register 0x35)
   Frame: FF FF ID 07 03 35 posL posH speedL speedH CHK
   ══════════════════════════════════════════════════════════════ */
static esp_err_t em3_set_position(uint8_t servo_id, uint16_t position, uint16_t speed)
{
    uint8_t frame[11];
    uint8_t bus_id = SERVO_BUS_ID(servo_id);

    frame[0] = 0xFF;              /* header1 */
    frame[1] = 0xFF;              /* header2 */
    frame[2] = bus_id;            /* servo ID */
    frame[3] = 0x07;              /* length (ID..CHK-1 = 7 bytes) */
    frame[4] = 0x03;              /* write command */
    frame[5] = 0x35;              /* target position register */
    frame[6] = position & 0xFF;   /* position low byte */
    frame[7] = (position >> 8) & 0xFF; /* position high byte */
    frame[8] = speed & 0xFF;      /* speed low byte */
    frame[9] = (speed >> 8) & 0xFF; /* speed high byte */
    frame[10] = calc_checksum(frame, 10);

    return send_with_oe(servo_id, frame, sizeof(frame));
}

/* ══════════════════════════════════════════════════════════════
   LX Protocol — set_position (command 0x01)
   Frame: 55 55 ID 07 01 posL posH timeL timeH CHK
   ══════════════════════════════════════════════════════════════ */
static esp_err_t lx_set_position(uint8_t servo_id, uint16_t position, uint16_t time_ms)
{
    uint8_t frame[10];
    uint8_t bus_id = SERVO_BUS_ID(servo_id);

    frame[0] = 0x55;              /* header1 */
    frame[1] = 0x55;              /* header2 */
    frame[2] = bus_id;            /* servo ID */
    frame[3] = 0x07;              /* length */
    frame[4] = 0x01;              /* position control command */
    frame[5] = position & 0xFF;   /* position low byte */
    frame[6] = (position >> 8) & 0xFF;
    frame[7] = time_ms & 0xFF;    /* time low byte */
    frame[8] = (time_ms >> 8) & 0xFF;
    frame[9] = calc_checksum(frame, 9);

    return send_with_oe(servo_id, frame, sizeof(frame));
}

/* ══════════════════════════════════════════════════════════════
   Public: set_position
   ══════════════════════════════════════════════════════════════ */
esp_err_t servo_driver_set_position(uint8_t servo_id, uint16_t position)
{
    if (servo_id >= SERVO_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_servo_types[servo_id] == SERVO_TYPE_EM3) {
        return em3_set_position(servo_id, position, 0);
    } else {
        /* LX: default 500ms move time */
        return lx_set_position(servo_id, position, 500);
    }
}

/* ══════════════════════════════════════════════════════════════
   Public: set_speed (EM3 only)
   ══════════════════════════════════════════════════════════════ */
esp_err_t servo_driver_set_speed(uint8_t servo_id, uint16_t speed)
{
    if (servo_id >= SERVO_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_servo_types[servo_id] == SERVO_TYPE_EM3) {
        /* EM3: write speed register 0x32 */
        uint8_t frame[8];
        uint8_t bus_id = SERVO_BUS_ID(servo_id);

        frame[0] = 0xFF;
        frame[1] = 0xFF;
        frame[2] = bus_id;
        frame[3] = 0x04;
        frame[4] = 0x03;  /* write */
        frame[5] = 0x32;  /* speed register */
        frame[6] = speed & 0xFF;
        frame[7] = calc_checksum(frame, 7);

        return send_with_oe(servo_id, frame, sizeof(frame));
    }

    /* LX: speed not supported in protocol, silently ignore */
    return ESP_OK;
}

/* ══════════════════════════════════════════════════════════════
   Public: move (with speed/time parameter)
   ══════════════════════════════════════════════════════════════ */
esp_err_t servo_driver_move(uint8_t servo_id, uint16_t position, uint16_t value)
{
    if (servo_id >= SERVO_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_servo_types[servo_id] == SERVO_TYPE_EM3) {
        return em3_set_position(servo_id, position, value);
    } else {
        return lx_set_position(servo_id, position, value);
    }
}

/* ══════════════════════════════════════════════════════════════
   Public: read_position
   ══════════════════════════════════════════════════════════════ */
esp_err_t servo_driver_read_position(uint8_t servo_id, uint16_t *position)
{
    if (servo_id >= SERVO_COUNT || position == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t tx_frame[8];
    uint8_t rx_buf[32];
    uint8_t bus_id = SERVO_BUS_ID(servo_id);

    if (s_servo_types[servo_id] == SERVO_TYPE_EM3) {
        /* EM3 read position:
         * Send: FF FF ID 04 02 4A 02 CHK
         * Response: FF FF ID 04 err posL posH CHK */
        tx_frame[0] = 0xFF;
        tx_frame[1] = 0xFF;
        tx_frame[2] = bus_id;
        tx_frame[3] = 0x04;
        tx_frame[4] = 0x02;       /* read command */
        tx_frame[5] = 0x4A;       /* position register address */
        tx_frame[6] = 0x02;       /* read 2 bytes */
        tx_frame[7] = calc_checksum(tx_frame, 7);

        for (int retry = 0; retry < 3; retry++) {
            vTaskDelay(pdMS_TO_TICKS(15));
            int n = servo_bus_transaction(UART_NUM_1, SERVO_OE1_GPIO,
                                          tx_frame, 8, rx_buf, sizeof(rx_buf), 5, 100);
            if (parse_em3_position_response(rx_buf, n, bus_id, position)) {
                return ESP_OK;
            }

            /* Last retry: print raw data for debugging */
            if (retry == 2) {
                printf("[EM3_DEBUG] direct read returned %d bytes:\n  ", n);
                for (int i = 0; i < n && i < 32; i++) {
                    printf("%02X ", rx_buf[i]);
                }
                printf("\n");
            }
        }
    } else {
        /* LX read position:
         * Send: 55 55 ID 03 1C CHK
         * Response: 55 55 ID 08 1C posL posH [6 bytes ...] CHK */
        tx_frame[0] = 0x55;
        tx_frame[1] = 0x55;
        tx_frame[2] = bus_id;
        tx_frame[3] = 0x03;
        tx_frame[4] = 0x1C;       /* read position command */
        tx_frame[5] = calc_checksum(tx_frame, 5);

        for (int retry = 0; retry < 3; retry++) {
            vTaskDelay(pdMS_TO_TICKS(10));
            int n = send_then_receive(servo_id, tx_frame, 6, rx_buf, sizeof(rx_buf), 50);
            if (parse_lx_position_response(rx_buf, n, bus_id, position)) {
                return ESP_OK;
            }
        }
    }

    return ESP_FAIL;
}

/**
 * @brief Read position of any LX servo by bus ID (bypasses init checks).
 *        Sends: 55 55 ID 03 1C CHK, parses response for position.
 * @param lx_bus_id  LX servo bus ID (1, 2, 3, or 5)
 * @param position   Output: position value (0-1000)
 * @return ESP_OK on success, ESP_FAIL on no response
 */
esp_err_t servo_driver_read_lx_bus_position(uint8_t lx_bus_id, uint16_t *position)
{
    if (position == NULL) return ESP_ERR_INVALID_ARG;

    uint8_t tx_frame[6];
    uint8_t rx_buf[32];

    tx_frame[0] = 0x55;
    tx_frame[1] = 0x55;
    tx_frame[2] = lx_bus_id;
    tx_frame[3] = 0x03;
    tx_frame[4] = 0x1C;       /* read position command */

    /* ~sum(lx_bus_id + 3 + 0x1C) */
    tx_frame[5] = (uint8_t)(~(lx_bus_id + 0x03 + 0x1C));

    for (int retry = 0; retry < 3; retry++) {
        vTaskDelay(pdMS_TO_TICKS(10));
        int n = servo_bus_transaction(UART_NUM_2, SERVO_OE2_GPIO,
                                      tx_frame, 6, rx_buf, sizeof(rx_buf), 5, 100);
        if (parse_lx_position_response(rx_buf, n, lx_bus_id, position)) {
            return ESP_OK;
        }
    }

    return ESP_FAIL;
}

/* ══════════════════════════════════════════════════════════════
   Public: torque_enable / torque_disable
   ══════════════════════════════════════════════════════════════ */
esp_err_t servo_driver_torque_enable(uint8_t servo_id)
{
    if (servo_id >= SERVO_COUNT) return ESP_ERR_INVALID_ARG;

    uint8_t frame[8];
    uint8_t bus_id = SERVO_BUS_ID(servo_id);

    if (s_servo_types[servo_id] == SERVO_TYPE_EM3) {
        /* EM3: write register 0x30 = 1 */
        frame[0] = 0xFF; frame[1] = 0xFF;
        frame[2] = bus_id;
        frame[3] = 0x04;
        frame[4] = 0x03;     /* write */
        frame[5] = 0x30;     /* torque enable register */
        frame[6] = 0x01;     /* enable */
        frame[7] = calc_checksum(frame, 7);
        return send_with_oe(servo_id, frame, sizeof(frame));
    } else {
        /* LX load command engages torque without changing its target position. */
        return servo_lx_enable_torque(bus_id);
    }
}

esp_err_t servo_driver_torque_disable(uint8_t servo_id)
{
    if (servo_id >= SERVO_COUNT) return ESP_ERR_INVALID_ARG;

    uint8_t frame[8];
    uint8_t bus_id = SERVO_BUS_ID(servo_id);

    if (s_servo_types[servo_id] == SERVO_TYPE_EM3) {
        /* EM3: write register 0x30 = 0 */
        frame[0] = 0xFF; frame[1] = 0xFF;
        frame[2] = bus_id;
        frame[3] = 0x04;
        frame[4] = 0x03;     /* write */
        frame[5] = 0x30;     /* torque enable register */
        frame[6] = 0x00;     /* disable */
        frame[7] = calc_checksum(frame, 7);
        return send_with_oe(servo_id, frame, sizeof(frame));
    } else {
        /* LX: unload command 0x1F
         * Frame: 55 55 ID 04 1F 00 CHK (7 bytes) */
        uint8_t lx_frame[7];
        lx_frame[0] = 0x55; lx_frame[1] = 0x55;
        lx_frame[2] = bus_id;
        lx_frame[3] = 0x04;
        lx_frame[4] = 0x1F;     /* unload command */
        lx_frame[5] = 0x00;
        lx_frame[6] = calc_checksum(lx_frame, 6);
        return send_with_oe(servo_id, lx_frame, sizeof(lx_frame));
    }
}

/* ══════════════════════════════════════════════════════════════
   Public: enable / disable (torque on the two index-based channels)
   ══════════════════════════════════════════════════════════════ */
esp_err_t servo_driver_enable(void)
{
    if (!s_driver_initialized) return ESP_ERR_INVALID_STATE;
    for (int i=0; i<SERVO_COUNT; ++i) {
        esp_err_t ret=servo_driver_torque_enable(i);
        if (ret!=ESP_OK) return ret;
        s_servo_enabled[i]=true;
    }
    return ESP_OK;
}

esp_err_t servo_driver_disable(void)
{
    if (!s_driver_initialized) return ESP_ERR_INVALID_STATE;
    for (int i=0; i<SERVO_COUNT; ++i) {
        esp_err_t ret=servo_driver_torque_disable(i);
        if (ret!=ESP_OK) return ret;
        s_servo_enabled[i]=false;
    }
    return ESP_OK;
}

servo_type_t servo_driver_get_type(uint8_t servo_id)
{
    if (servo_id >= SERVO_COUNT) return SERVO_TYPE_EM3;
    return s_servo_types[servo_id];
}

/* ══════════════════════════════════════════════════════════════
   Legacy EM3 API (bus-ID based)
   ══════════════════════════════════════════════════════════════ */

/* EM3 checksum: ~sum(bytes[2..len-1]) */
static uint8_t em3_checksum(const uint8_t *pkt, int len)
{
    uint32_t sum = 0;
    for (int i = 2; i < len - 1; i++) {
        sum += pkt[i];
    }
    return (uint8_t)(~sum);
}

/* Send raw bytes on EM3 UART with OE control */
static esp_err_t em3_bus_send(const uint8_t *data, int len)
{
    return servo_bus_send(UART_NUM_1, SERVO_OE1_GPIO, data, (uint16_t)len);
}

esp_err_t servo_em3_init(const servo_em3_config_t *config)
{
    if (s_em3_initialized) {
        return ESP_OK;
    }

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_driver_initialized) return ESP_ERR_INVALID_STATE;
    /* Idle receive direction is owned by the UART half-duplex driver. */
    s_em3_initialized = true;
    return ESP_OK;
}

esp_err_t servo_em3_deinit(void)
{
    s_em3_initialized = false;
    return ESP_OK;
}

esp_err_t servo_em3_move(uint8_t id, uint16_t pos, uint16_t speed)
{
    if (!s_em3_initialized) return ESP_ERR_INVALID_STATE;

    uint8_t pkt[11] = {0xFF, 0xFF, id, 0x07, 0x03, 0x35,
                       (uint8_t)(pos & 0xFF), (uint8_t)(pos >> 8),
                       (uint8_t)(speed & 0xFF), (uint8_t)(speed >> 8), 0};
    pkt[10] = em3_checksum(pkt, 11);
    return em3_bus_send(pkt, 11);
}

esp_err_t servo_em3_enable_torque(uint8_t id, uint8_t enable)
{
    if (!s_em3_initialized) return ESP_ERR_INVALID_STATE;

    uint8_t pkt[8] = {0xFF, 0xFF, id, 0x04, 0x03, 0x30, (uint8_t)(enable ? 1 : 0), 0};
    pkt[7] = em3_checksum(pkt, 8);
    return em3_bus_send(pkt, 8);
}

int16_t servo_em3_read_pos(uint8_t id)
{
    if (!s_em3_initialized) return -32768;

    uint8_t pkt[8] = {0xFF, 0xFF, id, 4, 0x02, 0x4A, 2, 0x00};
    pkt[7] = em3_checksum(pkt, 8);

    for (int retry = 0; retry < 3; retry++) {
        vTaskDelay(pdMS_TO_TICKS(15));
        uint8_t rx_buf[32];
        uint16_t position = 0;
        int len = servo_bus_transaction(UART_NUM_1, SERVO_OE1_GPIO,
                                        pkt, 8, rx_buf, sizeof(rx_buf), 5, 100);

        if (parse_em3_position_response(rx_buf, len, id, &position)) {
            if (retry>0) ESP_LOGI("servo_diag","QUERY recovered: EM3 id=%u attempt=%d position=%u",(unsigned)id,retry+1,(unsigned)position);
            return (int16_t)position;
        }
        if (retry==0 || retry==2) log_query(UART_NUM_1,pkt,8,rx_buf,len,retry+1,false);
    }

    return -32768;
}

/* ══════════════════════════════════════════════════════════════
   Legacy LX API (bus-ID based)
   ══════════════════════════════════════════════════════════════ */

/* LX checksum: ~sum(bytes[2..buf[3]+1]) */
static uint8_t lx_checksum(const uint8_t *buf)
{
    uint16_t sum = 0;
    for (int i = 2; i < buf[3] + 2; i++) {
        sum += buf[i];
    }
    return (uint8_t)(~sum);
}

/* Send raw bytes on LX UART with OE control */
static esp_err_t lx_bus_send(const uint8_t *data, int len)
{
    return servo_bus_send(UART_NUM_2, SERVO_OE2_GPIO, data, (uint16_t)len);
}

esp_err_t servo_lx_init(const servo_lx_config_t *config)
{
    if (s_lx_initialized) {
        return ESP_OK;
    }

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_driver_initialized) return ESP_ERR_INVALID_STATE;
    /* Idle receive direction is owned by the UART half-duplex driver. */
    s_lx_initialized = true;
    return ESP_OK;
}

esp_err_t servo_lx_deinit(void)
{
    s_lx_initialized = false;
    return ESP_OK;
}

esp_err_t servo_lx_move(uint8_t id, int16_t pos, uint16_t time)
{
    if (!s_lx_initialized) return ESP_ERR_INVALID_STATE;

    if (pos < 0) pos = 0;
    if (pos > 1000) pos = 1000;

    uint8_t pkt[10] = {0x55, 0x55, id, 7, 1,
                       (uint8_t)(pos & 0xFF), (uint8_t)(pos >> 8),
                       (uint8_t)(time & 0xFF), (uint8_t)(time >> 8), 0};
    pkt[9] = lx_checksum(pkt);
    return lx_bus_send(pkt, 10);
}

/* Explicit load: a previous unload must not leave gesture commands torque-free. */
esp_err_t servo_lx_enable_torque(uint8_t id)
{
    if (!s_lx_initialized) return ESP_ERR_INVALID_STATE;
    uint8_t pkt[7] = {0x55, 0x55, id, 4, 31, 1, 0};
    pkt[6] = lx_checksum(pkt);
    return lx_bus_send(pkt, 7);
}

esp_err_t servo_lx_move_group(uint8_t *ids, int16_t *pos, int count, uint16_t time)
{
    if (!s_lx_initialized) return ESP_ERR_INVALID_STATE;
    if (!ids || !pos || count < 1 || count > 4) return ESP_ERR_INVALID_ARG;
    for (int i=0; i<count; ++i) {
        esp_err_t ret = servo_lx_move(ids[i], pos[i], time);
        if (ret != ESP_OK) return ret;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_OK;
}

esp_err_t servo_lx_unload_all(uint8_t *ids, int count)
{
    if (!s_lx_initialized) return ESP_ERR_INVALID_STATE;
    if (!ids || count < 1 || count > 4) return ESP_ERR_INVALID_ARG;
    for (int i=0; i<count; ++i) {
        uint8_t pkt[7] = {0x55, 0x55, ids[i], 4, 31, 0, 0};
        pkt[6] = lx_checksum(pkt);
        esp_err_t ret = lx_bus_send(pkt, 7);
        if (ret != ESP_OK) return ret;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_OK;
}

int16_t servo_lx_read_pos(uint8_t id)
{
    if (!s_lx_initialized) return -32768;

    uint8_t tx_pkt[6] = {0x55, 0x55, id, 3, 0x1C, 0};
    tx_pkt[5] = lx_checksum(tx_pkt);

    for (int retry = 0; retry < 3; retry++) {
        vTaskDelay(pdMS_TO_TICKS(10));
        uint8_t rx_buf[32];
        uint16_t position = 0;
        int len = servo_bus_transaction(UART_NUM_2, SERVO_OE2_GPIO,
                                        tx_pkt, 6, rx_buf, sizeof(rx_buf), 10, 50);

        if (parse_lx_position_response(rx_buf, len, id, &position)) {
            if (retry>0) ESP_LOGI("servo_diag","QUERY recovered: LX id=%u attempt=%d position=%u",(unsigned)id,retry+1,(unsigned)position);
            return (int16_t)position;
        }
        if (retry==0 || retry==2) log_query(UART_NUM_2,tx_pkt,6,rx_buf,len,retry+1,true);
    }

    return -32768;
}
