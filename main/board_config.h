/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/**
 * @file board_config.h
 * @brief Centralized hardware and application configuration for ESP32-P4 MIPI DSI board
 *
 * This file consolidates all configuration parameters for the KD034WXFID001 3.4-inch
 * 480x480 MIPI DSI display with ST7701S driver IC and GT911 touch controller.
 *
 * Naming convention:
 *   - Board/hardware macros: BOARD_<MODULE>_<PARAM>
 *   - Application macros:    APP_<MODULE>_<PARAM>
 *   - All units are specified in the macro name (e.g., _HZ, _MS, _PX)
 *
 * Reference documents:
 *   - KD034WXFID001 MIPI.txt (panel init sequence)
 *   - lcd_full_text.txt (panel module manual)
 *   - st7701s_full_text.txt (driver IC datasheet)
 */

#include "driver/i2s_std.h"

#include <stdint.h>

/*===========================================================================*/
/* Section 1: LCD Panel Hardware Configuration (ST7701S + KD034WXFID001)     */
/*===========================================================================*/

/**
 * @name Panel Resolution
 * @brief Native resolution of the KD034WXFID001 panel
 * @{
 */
#define BOARD_LCD_H_RES 480 /*!< Horizontal resolution in pixels (fixed by panel) */
#define BOARD_LCD_V_RES 480 /*!< Vertical resolution in pixels (fixed by panel) */
/** @} */

/**
 * @name Panel Timing Parameters
 * @brief MIPI DSI video timing parameters from KD034WXFID001 MIPI.txt
 * @note These define the blanking periods. Adjust if display shifts or flickers.
 * @{
 */
#define BOARD_LCD_DPI_CLK_MHZ \
    20 /*!< DPI pixel clock in MHz. Range: 10-40. Higher = faster refresh but may cause timing issues */
#define BOARD_LCD_FRAME_RATE_HZ 60 /*!< Target refresh rate in Hz. Computed from DPI clock and timing */
#define BOARD_LCD_HSA 8            /*!< Horizontal sync pulse width in pixel clocks */
#define BOARD_LCD_HBP 56           /*!< Horizontal back porch in pixel clocks (sync to active area delay) */
#define BOARD_LCD_HFP 20           /*!< Horizontal front porch in pixel clocks (active area to sync delay) */
#define BOARD_LCD_VSA 10           /*!< Vertical sync pulse width in lines */
#define BOARD_LCD_VBP 60           /*!< Vertical back porch in lines */
#define BOARD_LCD_VFP 40           /*!< Vertical front porch in lines */
/** @} */

/**
 * @name Panel Color Format
 * @{
 */
#define BOARD_LCD_COLOR_FORMAT LCD_COLOR_FMT_RGB888   /*!< MIPI DSI color format: RGB888 (24-bit) */
#define BOARD_LCD_BITS_PER_PIXEL 24                   /*!< Bits per pixel (must match color format) */
#define BOARD_LCD_RGB_ORDER LCD_RGB_ELEMENT_ORDER_RGB /*!< RGB element order (not BGR) */
/** @} */

/**
 * @name Panel GPIO Assignments
 * @brief GPIO pins used for panel control signals
 * @{
 */
#define BOARD_LCD_RESET_GPIO GPIO_NUM_9 /*!< Panel hardware reset pin. Active low. Pulses during init */
/** @} */

/*===========================================================================*/
/* Section 2: MIPI DSI Interface Configuration                               */
/*===========================================================================*/

/**
 * @name MIPI DSI Bus
 * @brief D-PHY and lane configuration
 * @{
 */
#define BOARD_DSI_BUS_ID 0   /*!< DSI bus instance (ESP32-P4 has only 1) */
#define BOARD_DSI_LANE_NUM 2 /*!< Number of DSI data lanes. Range: 1-2. 2 lanes for full bandwidth */
#define BOARD_DSI_LANE_BITRATE_MBPS \
    500 /*!< Per-lane bit rate in Mbps. Range: 80-1500. 500Mbps supports 480x480@60Hz RGB888 */
#define BOARD_DSI_VIRTUAL_CHANNEL 0 /*!< DSI virtual channel ID (panel uses channel 0) */
#define BOARD_DSI_CMD_BITS 8        /*!< Command field width in bits (ST7701S uses 8-bit commands) */
#define BOARD_DSI_PARAM_BITS 8      /*!< Parameter field width in bits */
/** @} */

/**
 * @name MIPI DSI PHY Power (LDO)
 * @brief LDO regulator powering the D-PHY
 * @{
 */
#define BOARD_DSI_PHY_LDO_CHAN 3          /*!< LDO channel ID for DSI PHY power. ESP32-P4: channel 3 */
#define BOARD_DSI_PHY_LDO_VOLTAGE_MV 2500 /*!< LDO output voltage in mV. D-PHY requires 1.2V-2.5V, 2.5V recommended */
/** @} */

/**
 * @name DPI Frame Buffer
 * @{
 */
#define BOARD_DPI_FB_COUNT \
    2 /*!< Number of frame buffers. 1=single-buffer, 2=double-buffer. Set to 2 for flicker-free animation */
/** @} */

/*===========================================================================*/
/* Section 3: Backlight Configuration                                        */
/*===========================================================================*/

/**
 * @name Backlight Control
 * @brief Backlight is controlled via simple GPIO on/off (not PWM)
 * @note PWM parameters are defined for future use; currently using GPIO high/low
 * @{
 */
#define BOARD_BL_GPIO GPIO_NUM_0  /*!< Backlight enable GPIO pin. High = on, Low = off */
#define BOARD_BL_PWM_FREQ_HZ 5000 /*!< PWM frequency for brightness control (future use). Range: 500-20000 */
#define BOARD_BL_PWM_DUTY_RES LEDC_TIMER_8_BIT /*!< PWM duty resolution (256 levels) */
#define BOARD_BL_PWM_TIMER LEDC_TIMER_0        /*!< LEDC timer used for backlight PWM */
#define BOARD_BL_PWM_CHANNEL LEDC_CHANNEL_0    /*!< LEDC channel for backlight */
/** @} */

/*===========================================================================*/
/* Section 4: Touch Controller Configuration (GT911)                         */
/*===========================================================================*/

/**
 * @name I2C Bus for Touch
 * @brief I2C master bus configuration for GT911 communication
 * @{
 */
#define BOARD_TOUCH_I2C_PORT I2C_NUM_0      /*!< I2C port number */
#define BOARD_TOUCH_I2C_SDA_GPIO GPIO_NUM_7 /*!< I2C SDA pin */
#define BOARD_TOUCH_I2C_SCL_GPIO GPIO_NUM_8 /*!< I2C SCL pin */
#define BOARD_TOUCH_I2C_GLITCH_CNT 7        /*!< Glitch filter period. Range: 0-7. 7 = max filtering */
#define BOARD_TOUCH_I2C_PULLUP true /*!< Enable internal pull-up resistors. Set false if external pullups exist */
/** @} */

/**
 * @name GT911 Touch Controller
 * @brief GPIO and behavior configuration for GT911
 * @note GT911 I2C address is set by INT pin level during reset:
 *       - INT low during reset  -> address 0x5D (default, not used here)
 *       - INT high during reset -> address 0x14 (used in this project)
 * @{
 */
#define BOARD_TOUCH_RST_GPIO GPIO_NUM_34 /*!< GT911 reset pin. Active low */
#define BOARD_TOUCH_INT_GPIO GPIO_NUM_36 /*!< GT911 interrupt pin. Active low (falling edge = data ready) */
#define BOARD_TOUCH_RST_ACTIVE_LOW 0     /*!< Reset is active low (0 = reset asserted) */
#define BOARD_TOUCH_INT_ACTIVE_LOW 0     /*!< Interrupt is active low (0 = touch detected) */
#define BOARD_TOUCH_MAX_POINTS 5         /*!< Maximum simultaneous touch points supported by GT911 */
#define BOARD_TOUCH_SWAP_XY 0            /*!< Swap X/Y axes: 0=normal, 1=rotated 90° */
#define BOARD_TOUCH_MIRROR_X 0           /*!< Mirror X axis: 0=normal, 1=flipped */
#define BOARD_TOUCH_MIRROR_Y 0           /*!< Mirror Y axis: 0=normal, 1=flipped */
/** @} */

/*===========================================================================*/
/* Section 5: Gesture Recognition Parameters                                 */
/*===========================================================================*/

/**
 * @name Gesture Detection Thresholds
 * @brief Tunable parameters for gesture recognition sensitivity
 * @note Lower thresholds = more sensitive but more false positives
 * @{
 */
#define APP_GESTURE_SWIPE_THRESHOLD_PX 30 /*!< Min swipe distance (px). Range: 10-100. Lower = easier to trigger */
#define APP_GESTURE_LONG_PRESS_THRESHOLD_MS \
    500 /*!< Min long press duration in ms. Range: 300-2000. Lower = faster detection */
#define APP_GESTURE_PINCH_THRESHOLD \
    0.2f /*!< Min scale change for pinch. Range: 0.1-0.5. 0.2 = 20% size change required */
#define APP_GESTURE_ROTATION_THRESHOLD_DEG \
    20.0f /*!< Min rotation angle in degrees. Range: 5-45. Lower = more sensitive */
/** @} */

/**
 * @name Gesture History Buffer
 * @brief Touch point trajectory tracking buffer dimensions
 * @{
 */
#define APP_GESTURE_HISTORY_MAX_POINTS 5 /*!< Max tracked touch points (matches GT911 max) */
#define APP_GESTURE_HISTORY_MAX_LEN \
    20 /*!< History records per point. Range: 10-50. Higher = longer trajectory tracking */
/** @} */

/*===========================================================================*/
/* Section 6: Touch Game Parameters                                          */
/*===========================================================================*/

/**
 * @name Ball Game - Ball Properties
 * @{
 */
#define APP_GAME_BALL_INIT_X 240     /*!< Initial ball X position (screen center) */
#define APP_GAME_BALL_INIT_Y 240     /*!< Initial ball Y position (screen center) */
#define APP_GAME_BALL_INIT_RADIUS 25 /*!< Initial ball radius in pixels */
#define APP_GAME_BALL_MIN_RADIUS 10  /*!< Minimum ball radius (after pinch-in). Prevents ball from disappearing */
#define APP_GAME_BALL_MAX_RADIUS 50  /*!< Maximum ball radius (after pinch-out). Prevents ball from covering screen */
#define APP_GAME_BALL_RADIUS_STEP 5  /*!< Radius change per pinch gesture in pixels */
#define APP_GAME_BALL_INIT_COLOR COLOR_MAGENTA     /*!< Initial ball color */
#define APP_GAME_BALL_ROTATION_STEP_RAD (M_PI / 6) /*!< Rotation per gesture in radians (30°) */
#define APP_GAME_BALL_ROTATION_STEP_DEG 30.0f      /*!< Rotation per gesture in degrees (fallback) */
/** @} */

/**
 * @name Ball Game - Velocity
 * @{
 */
#define APP_GAME_BALL_VELOCITY 8 /*!< Ball velocity after swipe in pixels/frame. Range: 1-20. Higher = faster */
/** @} */

/**
 * @name Ball Game - Boundaries
 * @brief Game area rectangle on screen.
 *
 * The clear area is the rectangle erased each frame before redrawing the ball.
 * Ball center limits are computed dynamically as:
 *   x_min = CLEAR_X1 + radius, x_max = CLEAR_X2 - radius
 *   y_min = CLEAR_Y1 + radius, y_max = CLEAR_Y2 - radius
 * This guarantees the ball edge never exceeds the clear area, preventing
 * residual image artifacts when the ball is near a boundary.
 * @{
 */
#define APP_GAME_CLEAR_X1 15         /*!< Clear area left edge (ball never drawn left of this) */
#define APP_GAME_CLEAR_Y1 185        /*!< Clear area top edge */
#define APP_GAME_CLEAR_X2 465        /*!< Clear area right edge */
#define APP_GAME_CLEAR_Y2 395        /*!< Clear area bottom edge */
#define APP_GAME_AREA_OUTLINE_X1 10  /*!< Game area outline rect: top-left X */
#define APP_GAME_AREA_OUTLINE_Y1 170 /*!< Game area outline rect: top-left Y */
#define APP_GAME_AREA_OUTLINE_X2 470 /*!< Game area outline rect: bottom-right X */
#define APP_GAME_AREA_OUTLINE_Y2 400 /*!< Game area outline rect: bottom-right Y */
/** @} */

/**
 * @name Ball Game - Scoring
 * @{
 */
#define APP_GAME_SCORE_BOUNDARY 5 /*!< Points for hitting a boundary */
#define APP_GAME_SCORE_PINCH 1    /*!< Points for pinch gesture */
#define APP_GAME_SCORE_ROTATION 2 /*!< Points for rotation gesture */
/** @} */

/**
 * @name Ball Game - Loop Timing
 * @{
 */
#define APP_GAME_FRAME_DELAY_MS 20 /*!< Frame delay in ms. 20ms = 50 FPS */
#define APP_GAME_MAX_FRAMES 1000   /*!< Total frames before game ends (~20 seconds at 50 FPS) */
/** @} */

/**
 * @name Ball Game - Info Text Area
 * @brief Region below the game area for score/pause/velocity text.
 *        Cleared each frame to prevent text residual artifacts.
 * @{
 */
#define APP_GAME_INFO_Y1 405         /*!< Info area top edge */
#define APP_GAME_INFO_Y2 475         /*!< Info area bottom edge */
#define APP_GAME_INFO_SCORE_Y 410    /*!< Score text Y position */
#define APP_GAME_INFO_PAUSED_Y 435   /*!< Paused text Y position */
#define APP_GAME_INFO_VELOCITY_Y 460 /*!< Velocity text Y position */
/** @} */

/*===========================================================================*/
/* Section 7: GUI Color Palette (RGB888)                                     */
/*===========================================================================*/

/**
 * @name Color Definitions
 * @brief Standard RGB888 color constants (0xRRGGBB format)
 * @{
 */
#define COLOR_RED 0xFF0000     /*!< Pure red */
#define COLOR_GREEN 0x00FF00   /*!< Pure green */
#define COLOR_BLUE 0x0000FF    /*!< Pure blue */
#define COLOR_WHITE 0xFFFFFF   /*!< Pure white */
#define COLOR_BLACK 0x000000   /*!< Pure black */
#define COLOR_YELLOW 0xFFFF00  /*!< Pure yellow (red + green) */
#define COLOR_CYAN 0x00FFFF    /*!< Pure cyan (green + blue) */
#define COLOR_MAGENTA 0xFF00FF /*!< Pure magenta (red + blue) */
#define COLOR_ORANGE 0xFF8000  /*!< Orange */
#define COLOR_GRAY 0x808080    /*!< Medium gray (50% brightness) */
/** @} */

/*===========================================================================*/
/* Section 8: I2S Audio Configuration (INMP441 + MAX98357A)                  */
/*===========================================================================*/

/**
 * @name I2S Bus Configuration
 * @brief Full-duplex I2S bus shared by microphone and amplifier
 * @{
 */
#define BOARD_I2S_PORT I2S_NUM_0       /*!< I2S controller port */
#define BOARD_I2S_ROLE I2S_ROLE_MASTER /*!< ESP32-P4 as I2S master */
#define BOARD_I2S_SAMPLE_RATE 16000    /*!< Default sample rate in Hz (matches ESP-IDF official i2s_std example) */
#define BOARD_I2S_MCLK_MULTIPLE I2S_MCLK_MULTIPLE_256 /*!< MCLK = 256 * sample_rate */
#define BOARD_I2S_DATA_BIT_WIDTH \
    I2S_DATA_BIT_WIDTH_32BIT /*!< 32-bit required for INMP441 (24-bit data in 32-bit frame) */
/** @} */

/**
 * @name I2S GPIO Pins
 * @brief Pin assignments for I2S audio bus
 * @note BCLK and WS are shared between mic and amplifier
 * @{
 */
#define BOARD_I2S_BCLK_GPIO                                                                  \
    GPIO_NUM_32 /*!< Bit clock (shared) - actual hardware: MAX98357A BCLK=32, INMP441 SCK=32 \
                 */
#define BOARD_I2S_WS_GPIO \
    GPIO_NUM_33 /*!< Word select / LRC (shared) - actual hardware: MAX98357A LRC=33, INMP441 WS=33 */
#define BOARD_I2S_DOUT_GPIO GPIO_NUM_31 /*!< Data out → MAX98357A DIN - actual hardware: MAX98357A DIN=31 */
#define BOARD_I2S_DIN_GPIO GPIO_NUM_30  /*!< Data in ← INMP441 SD - actual hardware: INMP441 SD=30 */
/** @} */

/**
 * @name I2S Slot Configuration (shared by TX and RX in full-duplex)
 * @brief In full-duplex mode, TX and RX must use the same slot configuration.
 * Both channels share BCLK/WS, so slot_mode and slot_mask must be identical.
 * Using PHILIPS format + MONO + LEFT to match radar_test working configuration.
 * @{
 */
#define BOARD_I2S_SLOT_MODE I2S_SLOT_MODE_MONO /*!< Mono mode: matches radar_test working config */
#define BOARD_I2S_SLOT_MASK I2S_STD_SLOT_LEFT  /*!< Left slot only: matches radar_test working config */
/** @} */

/**
 * @name INMP441 Microphone Configuration
 * @{
 */
#define BOARD_MIC_CHANNEL BOARD_I2S_SLOT_MODE   /*!< Uses shared slot mode */
#define BOARD_MIC_SLOT_MASK BOARD_I2S_SLOT_MASK /*!< INMP441 L/R=GND outputs on left slot */
/** @} */

/**
 * @name MAX98357A Amplifier Configuration
 * @note SD pin is floating (default) → mixed mode: outputs (Left/2 + Right/2)
 *       To output only left channel, connect SD to VDD through 100K resistor (>1.4V)
 *       To shut down, connect SD to GND (<0.16V)
 * @{
 */
#define BOARD_AMP_CHANNEL BOARD_I2S_SLOT_MODE   /*!< Uses shared slot mode */
#define BOARD_AMP_SLOT_MASK BOARD_I2S_SLOT_MASK /*!< Use left slot for TX data */
/** @} */

/**
 * @name Audio DMA Configuration
 * @brief Matches radar_test working configuration
 * @{
 */
#define BOARD_AUDIO_DMA_BUF_COUNT 16 /*!< Number of DMA buffers (matches radar_test) */
#define BOARD_AUDIO_DMA_BUF_LEN 960  /*!< Samples per DMA buffer (matches radar_test) */
/** @} */

/**
 * @name LittleFS Storage Configuration
 * @{
 */
#define BOARD_AUDIO_PARTITION_LABEL "storage" /*!< Partition label in partitions.csv */
#define BOARD_AUDIO_MOUNT_POINT "/storage"    /*!< File system mount point */
/** @} */
