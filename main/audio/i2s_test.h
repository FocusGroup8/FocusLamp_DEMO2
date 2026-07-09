/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "i2s_driver.h"

/**
 * @brief I2S full-duplex test functions
 *
 * These functions verify I2S TX/RX channel functionality without
 * requiring external audio files or codecs.
 */

/**
 * @brief Test I2S channel creation and enable/disable
 *
 * Creates I2S channels, enables them, verifies operation,
 * then disables and deinits.
 *
 * @return ESP_OK if all tests passed
 */
esp_err_t i2s_test_channel_lifecycle(void);

/**
 * @brief Test TX output with sine wave
 *
 * Generates a 1kHz sine wave and outputs to MAX98357A amplifier.
 * Should produce audible tone if hardware is connected.
 *
 * @param duration_ms  Duration of test in milliseconds
 * @return ESP_OK on success
 */
esp_err_t i2s_test_tx_sine_wave(uint32_t duration_ms);

/**
 * @brief Test RX input from INMP441 microphone
 *
 * Reads audio data from microphone and prints amplitude statistics.
 * Speak into microphone to verify non-zero data.
 *
 * @param duration_ms  Duration of test in milliseconds
 * @return ESP_OK on success
 */
esp_err_t i2s_test_rx_microphone(uint32_t duration_ms);

/**
 * @brief Test full-duplex simultaneous TX and RX
 *
 * Runs TX sine wave output and RX microphone capture simultaneously.
 * Verifies that both channels operate without interference.
 *
 * @param duration_ms  Duration of test in milliseconds
 * @return ESP_OK on success
 */
esp_err_t i2s_test_full_duplex(uint32_t duration_ms);

/**
 * @brief I2S internal loopback diagnostic
 *
 * Creates I2S channels with DIN=DOUT for internal loopback.
 * Sends a known pattern and reads it back to verify the I2S peripheral
 * is outputting correct data on the DOUT pin.
 *
 * If loopback passes → issue is hardware (connection, MAX98357A, speaker)
 * If loopback fails → issue is software (driver, timing, configuration)
 *
 * @return ESP_OK on success
 */
esp_err_t i2s_test_loopback_diagnostic(void);

/**
 * @brief Run all I2S tests in sequence
 *
 * Executes: channel lifecycle → TX sine → RX mic → full-duplex
 *
 * @return ESP_OK if all tests passed
 */
esp_err_t i2s_test_run_all(void);