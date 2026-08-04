/*
 * system_config.h - System-level configuration macros for FocusLamp
 */

#pragma once
#ifndef __SYSTEM_CONFIG_H__
#define __SYSTEM_CONFIG_H__

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== System Version ===================== */
#define SYSTEM_VERSION_MAJOR        1
#define SYSTEM_VERSION_MINOR        0
#define SYSTEM_VERSION_PATCH        0
#define PROJECT_NAME                "FocusLamp"
#define HARDWARE_VERSION            "1.0"

/* ===================== FreeRTOS Task Stack Sizes ===================== */
#define TASK_STACK_MAIN             (4096)
#define TASK_STACK_LCD              (4096)
#define TASK_STACK_AUDIO            (3072)
#define TASK_STACK_SERVO            (3072)
#define TASK_STACK_TOUCH            (2048)
#define TASK_STACK_SENSOR           (2048)
#define TASK_STACK_RADAR            (2048)
#define TASK_STACK_UART_COMM        (3072)
#define TASK_STACK_EVENT_BUS        (2048)
#define TASK_STACK_APP_MANAGER      (4096)
#define TASK_STACK_ANIMATION        (3072)

/* ===================== Task Priorities ===================== */
#define PRIORITY_MAIN               (5)
#define PRIORITY_LCD                (6)
#define PRIORITY_AUDIO              (7)
#define PRIORITY_SERVO              (8)
#define PRIORITY_TOUCH              (4)
#define PRIORITY_SENSOR             (3)
#define PRIORITY_RADAR              (3)
#define PRIORITY_UART_COMM          (6)
#define PRIORITY_EVENT_BUS          (5)
#define PRIORITY_APP_MANAGER        (6)
#define PRIORITY_ANIMATION          (4)

/* ===================== Timeout Configurations (ms) ===================== */
#define TIMEOUT_TOUCH_LONG_PRESS    (800)
#define TIMEOUT_TOUCH_DOUBLE_CLICK  (300)
#define TIMEOUT_LCD_SLEEP           (30000)
#define TIMEOUT_SERVO_MOVE          (5000)
#define TIMEOUT_UART_RX             (1000)
#define TIMEOUT_UART_TX             (500)
#define TIMEOUT_APP_IDLE            (600000)  /* 10 min */
#define TIMEOUT_WATCHDOG            (5000)

/* ===================== I2S Configuration ===================== */
#define I2S_SAMPLE_RATE             (44100)
#define I2S_BITS_PER_SAMPLE         (16)
#define I2S_CHANNEL_NUM             (2)
#define I2S_DMA_BUF_COUNT           (8)
#define I2S_DMA_BUF_LEN             (1024)
#define I2S_MAIN_CLOCK              (12288000)  /* 12.288 MHz */

/* ===================== UART Configuration ===================== */
#define UART_COMM_BAUDRATE          (115200)
#define UART_COMM_DATA_BITS         (8)
#define UART_COMM_STOP_BITS         (1)
#define UART_COMM_PARITY            (0)         /* None */
#define UART_COMM_TX_BUF_SIZE       (2048)
#define UART_COMM_RX_BUF_SIZE       (2048)
#define UART_COMM_QUEUE_SIZE        (10)

/* ===================== LED Parameters ===================== */
#define LED_NUM_LEDS                (72)
#define LED_BRIGHTNESS_DEFAULT      (16)
#define LED_BRIGHTNESS_MAX          (30)
#define LED_REFRESH_INTERVAL_MS     (20)

/* ===================== Servo Parameters ===================== */
#define SERVO_BAUDRATE              (115200)
#define SERVO_PWM_FREQ              (50)        /* Hz */
#define SERVO_MIN_PULSE_US          (500)
#define SERVO_MAX_PULSE_US          (2500)

/* ===================== ADC Configuration ===================== */
#define ADC_LIGHT_SAMPLE_INTERVAL   (100)       /* ms */

#ifdef __cplusplus
}
#endif

#endif /* __SYSTEM_CONFIG_H__ */