/*
 * led_driver.c - Enhanced WS2812 LED strip driver implementation
 *
 * Integrates:
 * - RMT-based WS2812 control (from ws2812_module)
 * - Color buffer and brightness scaling
 * - State snapshot support (from led_controller)
 */

#include "led_driver.h"
#include "system_config.h"
#include "pin_config.h"
#include "bsp_gpio.h"

/* Brightness scaling denominator: brightness value range is 0-LED_BRIGHTNESS_MAX */
#define BRIGHTNESS_DIVISOR      LED_BRIGHTNESS_MAX

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"

static const char* TAG = "led_driver";

/* WS2812 timing constants */
#define LED_RMT_RESOLUTION_HZ   10000000
#define LED_RMT_T0H_TICKS       4
#define LED_RMT_T0L_TICKS       8
#define LED_RMT_T1H_TICKS       8
#define LED_RMT_T1L_TICKS       4
#define LED_RMT_RESET_TICKS     600
#define LED_RMT_MEM_BLOCK_SYMBOLS 64
#define LED_RMT_TRANS_QUEUE_DEPTH 4
#define LED_TX_TIMEOUT_MS       100

/* LED count from system config */
#define LED_COUNT               LED_NUM_LEDS
#define LED_STRIP_COUNT         LED_STRIP_TOTAL_COUNT  /* 物理灯带总长（超出部分保持熄灭） */
#define LED_BITS_PER_PIXEL      24

/* RMT handles */
static rmt_channel_handle_t    s_rmt_chan    = NULL;
static rmt_encoder_handle_t    s_rmt_encoder = NULL;

/* Internal color buffer (GRB order) */
static uint8_t* s_led_buffer = NULL;
static uint8_t  s_brightness = LED_BRIGHTNESS_DEFAULT;
static bool     s_initialized = false;

/* ===================== RMT WS2812 Transmit ===================== */

static esp_err_t led_driver_transmit_raw(const uint8_t* grb_data, size_t num_leds)
{
    if (!s_initialized || grb_data == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Build RMT symbols for WS2812 protocol */
    size_t total_bits = num_leds * LED_BITS_PER_PIXEL;
    size_t symbol_count = total_bits + 1; /* +1 for reset */
    rmt_symbol_word_t* symbols = (rmt_symbol_word_t*)malloc(symbol_count * sizeof(rmt_symbol_word_t));
    if (symbols == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t sym_idx = 0;
    for (size_t pixel = 0; pixel < num_leds; pixel++) {
        for (int bit = 7; bit >= 0; bit--) {
            bool bit_is_set = (grb_data[pixel * 3 + 0] >> bit) & 0x01;
            symbols[sym_idx++] = bit_is_set
                ? (rmt_symbol_word_t){ .level0 = 1, .duration0 = LED_RMT_T1H_TICKS,
                                       .level1 = 0, .duration1 = LED_RMT_T1L_TICKS }
                : (rmt_symbol_word_t){ .level0 = 1, .duration0 = LED_RMT_T0H_TICKS,
                                       .level1 = 0, .duration1 = LED_RMT_T0L_TICKS };
        }
        for (int bit = 7; bit >= 0; bit--) {
            bool bit_is_set = (grb_data[pixel * 3 + 1] >> bit) & 0x01;
            symbols[sym_idx++] = bit_is_set
                ? (rmt_symbol_word_t){ .level0 = 1, .duration0 = LED_RMT_T1H_TICKS,
                                       .level1 = 0, .duration1 = LED_RMT_T1L_TICKS }
                : (rmt_symbol_word_t){ .level0 = 1, .duration0 = LED_RMT_T0H_TICKS,
                                       .level1 = 0, .duration1 = LED_RMT_T0L_TICKS };
        }
        for (int bit = 7; bit >= 0; bit--) {
            bool bit_is_set = (grb_data[pixel * 3 + 2] >> bit) & 0x01;
            symbols[sym_idx++] = bit_is_set
                ? (rmt_symbol_word_t){ .level0 = 1, .duration0 = LED_RMT_T1H_TICKS,
                                       .level1 = 0, .duration1 = LED_RMT_T1L_TICKS }
                : (rmt_symbol_word_t){ .level0 = 1, .duration0 = LED_RMT_T0H_TICKS,
                                       .level1 = 0, .duration1 = LED_RMT_T0L_TICKS };
        }
    }

    /* Reset pulse */
    symbols[sym_idx] = (rmt_symbol_word_t){
        .level0 = 0, .duration0 = LED_RMT_RESET_TICKS,
        .level1 = 0, .duration1 = 0,
    };

    const rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    esp_err_t ret = rmt_transmit(s_rmt_chan, s_rmt_encoder, symbols,
                                 symbol_count * sizeof(rmt_symbol_word_t), &tx_cfg);
    if (ret == ESP_OK) {
        ret = rmt_tx_wait_all_done(s_rmt_chan, LED_TX_TIMEOUT_MS);
    }

    free(symbols);
    return ret;
}

/* ===================== Public API ===================== */

esp_err_t led_driver_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "LED driver already initialized");
        return ESP_OK;
    }

    /* Allocate color buffer */
    s_led_buffer = (uint8_t*)calloc(LED_STRIP_COUNT * 3, sizeof(uint8_t));
    if (s_led_buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Initialize RMT TX channel for WS2812 */
    const rmt_tx_channel_config_t tx_chan_cfg = {
        .gpio_num           = LED_DIN_GPIO,
        .clk_src            = RMT_CLK_SRC_DEFAULT,
        .resolution_hz      = LED_RMT_RESOLUTION_HZ,
        .mem_block_symbols  = LED_RMT_MEM_BLOCK_SYMBOLS,
        .trans_queue_depth  = LED_RMT_TRANS_QUEUE_DEPTH,
        .flags.invert_out   = false,
        .flags.with_dma     = false,
        .flags.io_loop_back = false,
        .flags.io_od_mode   = false,
    };

    esp_err_t ret = rmt_new_tx_channel(&tx_chan_cfg, &s_rmt_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create RMT TX channel: %s", esp_err_to_name(ret));
        free(s_led_buffer);
        s_led_buffer = NULL;
        return ret;
    }

    const rmt_copy_encoder_config_t copy_enc_cfg = {};
    ret = rmt_new_copy_encoder(&copy_enc_cfg, &s_rmt_encoder);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create RMT encoder: %s", esp_err_to_name(ret));
        rmt_del_channel(s_rmt_chan);
        s_rmt_chan = NULL;
        free(s_led_buffer);
        s_led_buffer = NULL;
        return ret;
    }

    ret = rmt_enable(s_rmt_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable RMT channel: %s", esp_err_to_name(ret));
        rmt_del_encoder(s_rmt_encoder);
        rmt_del_channel(s_rmt_chan);
        s_rmt_encoder = NULL;
        s_rmt_chan    = NULL;
        free(s_led_buffer);
        s_led_buffer = NULL;
        return ret;
    }

    s_initialized = true;
    s_brightness  = LED_BRIGHTNESS_DEFAULT;
    memset(s_led_buffer, 0, LED_STRIP_COUNT * 3);

    ESP_LOGI(TAG, "LED driver initialized (GPIO=%d, count=%d, strip=%d)", LED_DIN_GPIO, LED_COUNT, LED_STRIP_COUNT);
    return ESP_OK;
}

esp_err_t led_driver_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = rmt_disable(s_rmt_chan);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to disable RMT: %s", esp_err_to_name(ret));
    }

    if (s_rmt_encoder) {
        rmt_del_encoder(s_rmt_encoder);
        s_rmt_encoder = NULL;
    }
    if (s_rmt_chan) {
        rmt_del_channel(s_rmt_chan);
        s_rmt_chan = NULL;
    }

    if (s_led_buffer) {
        free(s_led_buffer);
        s_led_buffer = NULL;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "LED driver deinitialized");
    return ESP_OK;
}

esp_err_t led_driver_set_color(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_initialized || s_led_buffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (index >= LED_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Store in GRB order (WS2812 native) */
    s_led_buffer[index * 3 + 0] = g;
    s_led_buffer[index * 3 + 1] = r;
    s_led_buffer[index * 3 + 2] = b;

    return ESP_OK;
}

esp_err_t led_driver_set_all(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_initialized || s_led_buffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    for (uint8_t i = 0; i < LED_COUNT; i++) {
        s_led_buffer[i * 3 + 0] = g;
        s_led_buffer[i * 3 + 1] = r;
        s_led_buffer[i * 3 + 2] = b;
    }

    return ESP_OK;
}

esp_err_t led_driver_show(void)
{
    if (!s_initialized || s_led_buffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Apply brightness scaling before transmit */
    uint8_t* scaled_buffer = (uint8_t*)malloc(LED_STRIP_COUNT * 3);
    if (scaled_buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    for (uint8_t i = 0; i < LED_STRIP_COUNT; i++) {
        uint8_t g = s_led_buffer[i * 3 + 0];
        uint8_t r = s_led_buffer[i * 3 + 1];
        uint8_t b = s_led_buffer[i * 3 + 2];
        scaled_buffer[i * 3 + 0] = (uint8_t)(((uint16_t)g * s_brightness) / BRIGHTNESS_DIVISOR);
        scaled_buffer[i * 3 + 1] = (uint8_t)(((uint16_t)r * s_brightness) / BRIGHTNESS_DIVISOR);
        scaled_buffer[i * 3 + 2] = (uint8_t)(((uint16_t)b * s_brightness) / BRIGHTNESS_DIVISOR);
    }

    /* GRB data is already in GRB order in the buffer */
    esp_err_t ret = led_driver_transmit_raw(scaled_buffer, LED_STRIP_COUNT);
    free(scaled_buffer);
    return ret;
}

void led_driver_set_brightness(uint8_t brightness)
{
    s_brightness = brightness;
}

uint8_t led_driver_get_brightness(void)
{
    return s_brightness;
}

esp_err_t led_driver_clear(void)
{
    if (!s_initialized || s_led_buffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(s_led_buffer, 0, LED_STRIP_COUNT * 3);
    return led_driver_show();
}

bool led_driver_is_initialized(void)
{
    return s_initialized;
}

esp_err_t led_driver_get_snapshot(led_module_snapshot_t* snapshot)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    snapshot->initialized = s_initialized;
    snapshot->brightness  = s_brightness;

    /* Read first LED color as snapshot */
    if (s_led_buffer && LED_COUNT > 0) {
        snapshot->color.red   = s_led_buffer[1];
        snapshot->color.green = s_led_buffer[0];
        snapshot->color.blue  = s_led_buffer[2];
    } else {
        snapshot->color = (ws2812_rgb_t){0};
    }

    return ESP_OK;
}

void led_driver_scale_brightness(uint8_t r, uint8_t g, uint8_t b,
                                 uint8_t brightness,
                                 uint8_t* out_r, uint8_t* out_g, uint8_t* out_b)
{
    if (out_r) *out_r = (uint8_t)(((uint16_t)r * brightness) / BRIGHTNESS_DIVISOR);
    if (out_g) *out_g = (uint8_t)(((uint16_t)g * brightness) / BRIGHTNESS_DIVISOR);
    if (out_b) *out_b = (uint8_t)(((uint16_t)b * brightness) / BRIGHTNESS_DIVISOR);
}
