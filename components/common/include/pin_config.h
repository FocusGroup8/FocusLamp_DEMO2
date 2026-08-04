/*
 * pin_config.h - Pin configuration for FocusLamp hardware
 */

#pragma once
#ifndef __PIN_CONFIG_H__
#define __PIN_CONFIG_H__

#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== Power Management ===================== */
#define ESP_EN_GPIO          GPIO_NUM_45  /* M3-45 */
/* 3.3V: M3-1, M3-3 */
/* 5V:   M3-41, M3-43 */
/* GND:  M3-39, M3-40, M3-79, M3-80 */

/* ===================== RGB Ambient LED ===================== */
#define LED_DIN_GPIO         GPIO_NUM_6   /* M3-24 */

/* ===================== Ambient Light Sensor ===================== */
#define TEMT_OUT_GPIO        GPIO_NUM_21  /* M3-33 */

/* ===================== Touch Sensors ===================== */
#define TTP_A_GPIO           GPIO_NUM_9   /* M3-34 */
#define TTP_B_GPIO           GPIO_NUM_22  /* M3-35 */
#define TTP_C_GPIO           GPIO_NUM_10  /* M3-36 */
#define TTP_D_GPIO           GPIO_NUM_23  /* M3-37 */

/* ===================== LCD Display ===================== */
#ifndef LCD_BL_GPIO
#define LCD_BL_GPIO          GPIO_NUM_51  /* M3-54 */
#endif
#ifndef LCD_RST_GPIO
#define LCD_RST_GPIO         GPIO_NUM_NC  /* Set to actual LCD RES GPIO when wired */
#endif
#define LCD_SCL_GPIO         GPIO_NUM_50  /* M3-55 */
#define LCD_SDA_GPIO         GPIO_NUM_36  /* M3-56 */
#define LCD_DC_GPIO          GPIO_NUM_49  /* M3-57 */
#define LCD_CS_GPIO          GPIO_NUM_34  /* M3-58 */

/* ===================== Audio I2S ===================== */
#define AUD_LRC_GPIO         GPIO_NUM_33  /* M3-59 */
#define AUD_BCLK_GPIO        GPIO_NUM_32  /* M3-61 */
#define AUD_DIN_GPIO         GPIO_NUM_31  /* M3-63 */
#define AUD_SD_GPIO          GPIO_NUM_30  /* M3-65 */

/* ===================== Servo Communication 1 (EM3 - UART1) ===================== */
#define SERVO_TXD1_GPIO      GPIO_NUM_8   /* EM3 TX */
#define SERVO_RXD1_GPIO      GPIO_NUM_2   /* EM3 RX */
#define SERVO_OE1_GPIO       GPIO_NUM_11  /* EM3 OE */

/* ===================== Servo Communication 2 (LX - UART2) ===================== */
#define SERVO_TXD2_GPIO      GPIO_NUM_3   /* LX TX */
#define SERVO_RXD2_GPIO      GPIO_NUM_20  /* LX RX */
#define SERVO_OE2_GPIO       GPIO_NUM_4   /* LX OE */

/* ===================== Radar ===================== */
#define RADAR_RX_GPIO        GPIO_NUM_7   /* M3-25 */
#define RADAR_TX_GPIO        GPIO_NUM_1   /* M3-26 */

/* ===================== Dual-Board UART2 ===================== */
#define UART2_TX_GPIO        GPIO_NUM_54  /* M3-42 */
#define UART2_RX_GPIO        GPIO_NUM_53  /* M3-44 */

#ifdef __cplusplus
}
#endif

#endif /* __PIN_CONFIG_H__ */