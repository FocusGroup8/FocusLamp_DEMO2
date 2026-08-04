/*
 * servo_driver.h - Serial bus servo driver for FocusLamp
 * Supports EM3 (0xFF header) and LX (0x55 header) protocols
 * via UART with OE pin half-duplex control.
 */

#pragma once
#ifndef __SERVO_DRIVER_H__
#define __SERVO_DRIVER_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/uart.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * TODO: Move SERVO_COUNT to system_config.h when finalized.
 * Currently two servo channels are defined in pin_config.h (SERVO_TXD1/RXD1, SERVO_TXD2/RXD2).
 */
#define SERVO_COUNT  2

/** Servo protocol type */
typedef enum {
    SERVO_TYPE_EM3 = 0,  /**< Protocol with 0xFF 0xFF frame header, register-based */
    SERVO_TYPE_LX  = 1,  /**< Protocol with 0x55 0x55 frame header, command-based */
} servo_type_t;

/**
 * @brief Initialize servo driver.
 *        Configures UARTs for servo communication via bsp_uart_init().
 *        Sets up OE (output enable) pins for each servo channel.
 * @return esp_err_t
 */
esp_err_t servo_driver_init(void);

/**
 * @brief Send a raw command to servo via UART1.
 * @param data  Pointer to command data buffer
 * @param len   Data length in bytes
 * @return esp_err_t
 */
esp_err_t servo_driver_send_command(const uint8_t *data, uint16_t len);

/**
 * @brief Read servo response from UART1.
 * @param buffer   Pointer to receive buffer
 * @param timeout  Timeout in milliseconds
 * @return int     Number of bytes read, or -1 on error
 */
int servo_driver_read_response(uint8_t *buffer, uint16_t timeout);

/**
 * @brief Set servo position.
 * @param servo_id  Servo ID (0-based, up to SERVO_COUNT - 1)
 * @param position  Target position (EM3: 0-3000, LX: 0-1000)
 * @return esp_err_t
 */
esp_err_t servo_driver_set_position(uint8_t servo_id, uint16_t position);

/**
 * @brief Set servo movement speed (EM3 only, ignored for LX).
 * @param servo_id  Servo ID (0-based, up to SERVO_COUNT - 1)
 * @param speed     Speed value (0-1000)
 * @return esp_err_t
 */
esp_err_t servo_driver_set_speed(uint8_t servo_id, uint16_t speed);

/**
 * @brief Move servo with speed (EM3) or time (LX) parameter.
 *        LX uses time_ms for movement duration; EM3 uses speed internally.
 * @param servo_id  Servo ID (0-based)
 * @param position  Target position
 * @param value     Speed (EM3: 0-1000) or time_ms (LX: 0-65535)
 * @return esp_err_t
 */
esp_err_t servo_driver_move(uint8_t servo_id, uint16_t position, uint16_t value);

/**
 * @brief Read current servo position.
 * @param servo_id  Servo ID (0-based)
 * @param position  Output: current position value
 * @return esp_err_t
 */
esp_err_t servo_driver_read_position(uint8_t servo_id, uint16_t *position);

/**
 * @brief Read position of any LX servo on the bus by its actual bus ID.
 *        Unlike servo_driver_read_position, this bypasses the index mapping
 *        and any init checks, reading directly from the LX UART.
 * @param lx_bus_id  LX servo bus ID (1, 2, 3, or 5)
 * @param position   Output: position value (0-1000)
 * @return ESP_OK on success, ESP_FAIL on no response
 */
esp_err_t servo_driver_read_lx_bus_position(uint8_t lx_bus_id, uint16_t *position);

/**
 * @brief Enable torque on a servo.
 * @param servo_id  Servo ID (0-based)
 * @return esp_err_t
 */
esp_err_t servo_driver_torque_enable(uint8_t servo_id);

/**
 * @brief Disable torque on a servo (release).
 * @param servo_id  Servo ID (0-based)
 * @return esp_err_t
 */
esp_err_t servo_driver_torque_disable(uint8_t servo_id);

/**
 * @brief Enable all servos (set OE pins high).
 * @return esp_err_t
 */
esp_err_t servo_driver_enable(void);

/**
 * @brief Disable all servos (set OE pins low).
 * @return esp_err_t
 */
esp_err_t servo_driver_disable(void);

/**
 * @brief Get the protocol type of a servo.
 * @param servo_id  Servo ID (0-based)
 * @return servo_type_t
 */
servo_type_t servo_driver_get_type(uint8_t servo_id);

/* ==================== Legacy EM3 Servo API (bus-ID based) ==================== */

typedef struct {
    uart_port_t uart_num;
    int         tx_pin;
    int         rx_pin;
    int         baud_rate;
} servo_em3_config_t;

esp_err_t servo_em3_init(const servo_em3_config_t *config);
esp_err_t servo_em3_deinit(void);
void      servo_em3_move(uint8_t id, uint16_t pos, uint16_t speed);
void      servo_em3_enable_torque(uint8_t id, uint8_t enable);
int16_t   servo_em3_read_pos(uint8_t id);

/* ==================== Legacy LX Servo API (bus-ID based) ==================== */

typedef struct {
    uart_port_t uart_num;
    int         tx_pin;
    int         rx_pin;
    int         baud_rate;
} servo_lx_config_t;

esp_err_t servo_lx_init(const servo_lx_config_t *config);
esp_err_t servo_lx_deinit(void);
void      servo_lx_move(uint8_t id, int16_t pos, uint16_t time);
void      servo_lx_move_group(uint8_t *ids, int16_t *pos, int count, uint16_t time);
void      servo_lx_unload_all(uint8_t *ids, int count);
int16_t   servo_lx_read_pos(uint8_t id);

#ifdef __cplusplus
}
#endif

#endif /* __SERVO_DRIVER_H__ */
