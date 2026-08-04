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
#include "pin_config.h"
#include "bsp_uart.h"
#include "bsp_gpio.h"
#include "system_config.h"

#include <string.h>
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
static bool s_em3_initialized = false;
static bool s_lx_initialized = false;

static bool lock_servo_bus(uart_port_t uart)
{
    if (uart < 0 || uart > UART_NUM_2) {
        return false;
    }

    SemaphoreHandle_t mutex = s_uart_mutex[uart];
    if (mutex == NULL) {
        return true;
    }

    return xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE;
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
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!lock_servo_bus(uart)) {
        return ESP_FAIL;
    }

    uart_flush(uart);  /* discard self-echoed bytes from previous transactions */
    bsp_gpio_set_level(oe, 1);
    esp_err_t ret = bsp_uart_write_bytes(uart, data, len);
    vTaskDelay(pdMS_TO_TICKS(3));  /* wait for TX to complete before disabling OE */
    bsp_gpio_set_level(oe, 0);

    unlock_servo_bus(uart);
    return ret;
}

static int servo_bus_transaction(uart_port_t uart, gpio_num_t oe, const uint8_t *tx_data, uint16_t tx_len,
                                 uint8_t *rx_buf, uint16_t rx_len, uint16_t settle_ms, uint16_t timeout_ms)
{
    if (tx_data == NULL || rx_buf == NULL) {
        return -1;
    }

    if (!lock_servo_bus(uart)) {
        return -1;
    }

    uart_flush(uart);  /* discard self-echoed bytes from previous transactions */
    bsp_gpio_set_level(oe, 1);
    esp_err_t ret = bsp_uart_write_bytes(uart, tx_data, tx_len);
    /* bsp_uart_write_bytes() already waits for TX completion; switch back to RX immediately
     * to avoid missing fast servo responses. */
    bsp_gpio_set_level(oe, 0);
    if (ret != ESP_OK) {
        unlock_servo_bus(uart);
        return -1;
    }

    if (settle_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(settle_ms));
    }

    int n = bsp_uart_read_bytes(uart, rx_buf, rx_len, pdMS_TO_TICKS(timeout_ms));

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
    for (int i = 0; i < SERVO_COUNT; i++) {
        bsp_gpio_set_level(servo_oe_pin(s_servo_types[i]), 0);
        s_servo_enabled[i] = false;
    }

    /* Create UART mutexes for half-duplex bus arbitration */
    for (int i = 0; i <= UART_NUM_2; i++) {
        s_uart_mutex[i] = xSemaphoreCreateMutex();
    }

    return ESP_OK;
}

esp_err_t servo_driver_send_command(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0) return ESP_ERR_INVALID_ARG;
    /* Default to EM3 UART for raw command */
    return bsp_uart_write_bytes(UART_NUM_1, data, len);
}

int servo_driver_read_response(uint8_t *buffer, uint16_t timeout)
{
    if (buffer == NULL) return -1;
    /* Default to EM3 UART for response */
    return bsp_uart_read_bytes(UART_NUM_1, buffer, 16, pdMS_TO_TICKS(timeout));
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
        /* LX: no explicit torque enable command; use move to 0 to engage */
        return ESP_OK;
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
   Public: enable / disable (OE only)
   ══════════════════════════════════════════════════════════════ */
esp_err_t servo_driver_enable(void)
{
    for (int i = 0; i < SERVO_COUNT; i++) {
        bsp_gpio_set_level(servo_oe_pin(s_servo_types[i]), 1);
        s_servo_enabled[i] = true;
    }
    return ESP_OK;
}

esp_err_t servo_driver_disable(void)
{
    for (int i = 0; i < SERVO_COUNT; i++) {
        bsp_gpio_set_level(servo_oe_pin(s_servo_types[i]), 0);
        s_servo_enabled[i] = false;
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
static void em3_bus_send(const uint8_t *data, int len)
{
    (void)servo_bus_send(UART_NUM_1, SERVO_OE1_GPIO, data, (uint16_t)len);
}

esp_err_t servo_em3_init(const servo_em3_config_t *config)
{
    if (s_em3_initialized) {
        return ESP_OK;
    }

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* UART is initialized by BSP, just set OE low */
    bsp_gpio_set_level(SERVO_OE1_GPIO, 0);
    s_em3_initialized = true;
    return ESP_OK;
}

esp_err_t servo_em3_deinit(void)
{
    s_em3_initialized = false;
    return ESP_OK;
}

void servo_em3_move(uint8_t id, uint16_t pos, uint16_t speed)
{
    if (!s_em3_initialized) return;

    uint8_t pkt[11] = {0xFF, 0xFF, id, 0x07, 0x03, 0x35,
                       (uint8_t)(pos & 0xFF), (uint8_t)(pos >> 8),
                       (uint8_t)(speed & 0xFF), (uint8_t)(speed >> 8), 0};
    pkt[10] = em3_checksum(pkt, 11);
    em3_bus_send(pkt, 11);
}

void servo_em3_enable_torque(uint8_t id, uint8_t enable)
{
    if (!s_em3_initialized) return;

    uint8_t pkt[8] = {0xFF, 0xFF, id, 0x04, 0x03, 0x30, (uint8_t)(enable ? 1 : 0), 0};
    pkt[7] = em3_checksum(pkt, 8);
    em3_bus_send(pkt, 8);
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
            return (int16_t)position;
        }
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
static void lx_bus_send(const uint8_t *data, int len)
{
    (void)servo_bus_send(UART_NUM_2, SERVO_OE2_GPIO, data, (uint16_t)len);
}

esp_err_t servo_lx_init(const servo_lx_config_t *config)
{
    if (s_lx_initialized) {
        return ESP_OK;
    }

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* UART is initialized by BSP, just set OE low */
    bsp_gpio_set_level(SERVO_OE2_GPIO, 0);
    s_lx_initialized = true;
    return ESP_OK;
}

esp_err_t servo_lx_deinit(void)
{
    s_lx_initialized = false;
    return ESP_OK;
}

void servo_lx_move(uint8_t id, int16_t pos, uint16_t time)
{
    if (!s_lx_initialized) return;

    if (pos < 0) pos = 0;
    if (pos > 1000) pos = 1000;

    uint8_t pkt[10] = {0x55, 0x55, id, 7, 1,
                       (uint8_t)(pos & 0xFF), (uint8_t)(pos >> 8),
                       (uint8_t)(time & 0xFF), (uint8_t)(time >> 8), 0};
    pkt[9] = lx_checksum(pkt);
    lx_bus_send(pkt, 10);
}

void servo_lx_move_group(uint8_t *ids, int16_t *pos, int count, uint16_t time)
{
    if (!s_lx_initialized) return;

    for (int i = 0; i < count; i++) {
        servo_lx_move(ids[i], pos[i], time);
        vTaskDelay(pdMS_TO_TICKS(20));  /* allow bus to settle between commands */
    }
}

void servo_lx_unload_all(uint8_t *ids, int count)
{
    if (!s_lx_initialized) return;

    for (int i = 0; i < count; i++) {
        uint8_t pkt[7] = {0x55, 0x55, ids[i], 4, 31, 0, 0};
        pkt[6] = lx_checksum(pkt);
        lx_bus_send(pkt, 7);
        vTaskDelay(pdMS_TO_TICKS(20));  /* allow bus to settle between commands */
    }
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
            return (int16_t)position;
        }
    }

    return -32768;
}
