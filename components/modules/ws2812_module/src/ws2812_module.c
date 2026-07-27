#include "ws2812_module.h"

#if (WS2812_MODULE_ENABLE == 1)

#include <string.h>

#include "esp_log.h"

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"

static const char* TAG = "ws2812_module";

#define WS2812_BITS_PER_LED 24
#define WS2812_RESET_TICKS 600
#define WS2812_T0H_TICKS 4
#define WS2812_T0L_TICKS 8
#define WS2812_T1H_TICKS 8
#define WS2812_T1L_TICKS 4
#define WS2812_MEM_BLOCK_SYMBOLS 64
#define WS2812_TRANS_QUEUE_DEPTH 4
#define WS2812_TX_TIMEOUT_MS 100
#define WS2812_RMT_RESOLUTION_HZ 10000000

static rmt_channel_handle_t s_ws2812_chan        = NULL;
static rmt_encoder_handle_t s_ws2812_encoder     = NULL;
static bool                 s_ws2812_initialized = false;
static uint8_t              s_ws2812_brightness  = 0xFF;
static ws2812_rgb_t         s_ws2812_color       = {
    .red   = 0,
    .green = 0,
    .blue  = 255,
};

static esp_err_t ws2812_module_transmit_color(const ws2812_rgb_t* color)
{
    if (!s_ws2812_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (color == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t grb[3] = {
        color->green,
        color->red,
        color->blue,
    };
    rmt_symbol_word_t symbols[WS2812_BITS_PER_LED + 1] = {0};
    size_t            symbol_index                     = 0;

    for (size_t byte_index = 0; byte_index < sizeof(grb); ++byte_index)
    {
        for (int bit = 7; bit >= 0; --bit)
        {
            const bool bit_is_set = (grb[byte_index] >> bit) & 0x01;
            symbols[symbol_index++] = bit_is_set
                ? (rmt_symbol_word_t) {
                    .level0 = 1,
                    .duration0 = WS2812_T1H_TICKS,
                    .level1 = 0,
                    .duration1 = WS2812_T1L_TICKS,
                }
                : (rmt_symbol_word_t) {
                    .level0 = 1,
                    .duration0 = WS2812_T0H_TICKS,
                    .level1 = 0,
                    .duration1 = WS2812_T0L_TICKS,
                };
        }
    }

    symbols[symbol_index] = (rmt_symbol_word_t){
        .level0    = 0,
        .duration0 = WS2812_RESET_TICKS,
        .level1    = 0,
        .duration1 = 0,
    };

    const rmt_transmit_config_t transmit_config = {
        .loop_count = 0,
    };

    esp_err_t ret =
        rmt_transmit(s_ws2812_chan, s_ws2812_encoder, symbols, sizeof(symbols), &transmit_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to transmit WS2812 symbols: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = rmt_tx_wait_all_done(s_ws2812_chan, WS2812_TX_TIMEOUT_MS);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed waiting for WS2812 TX done: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

static uint8_t ws2812_module_scale_channel(uint8_t value, uint8_t brightness)
{
    return (uint8_t)((value * brightness) / 255U);
}

static ws2812_rgb_t ws2812_module_apply_brightness(const ws2812_rgb_t* color, uint8_t brightness)
{
    ws2812_rgb_t scaled = {0};

    if (color == NULL)
    {
        return scaled;
    }

    scaled.red   = ws2812_module_scale_channel(color->red, brightness);
    scaled.green = ws2812_module_scale_channel(color->green, brightness);
    scaled.blue  = ws2812_module_scale_channel(color->blue, brightness);
    return scaled;
}

esp_err_t ws2812_module_init(void)
{
    if (s_ws2812_initialized)
    {
        ESP_LOGW(TAG, "WS2812 module already initialized");
        return ESP_OK;
    }

    if (WS2812_MODULE_LED_COUNT != 1)
    {
        ESP_LOGE(TAG, "Current module implementation only supports one LED");
        return ESP_ERR_NOT_SUPPORTED;
    }

    const rmt_tx_channel_config_t tx_channel_config = {
        .gpio_num           = WS2812_MODULE_GPIO,
        .clk_src            = RMT_CLK_SRC_DEFAULT,
        .resolution_hz      = WS2812_RMT_RESOLUTION_HZ,
        .mem_block_symbols  = WS2812_MEM_BLOCK_SYMBOLS,
        .trans_queue_depth  = WS2812_TRANS_QUEUE_DEPTH,
        .flags.invert_out   = false,
        .flags.with_dma     = false,
        .flags.io_loop_back = false,
        .flags.io_od_mode   = false,
    };

    esp_err_t ret = rmt_new_tx_channel(&tx_channel_config, &s_ws2812_chan);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create RMT TX channel: %s", esp_err_to_name(ret));
        return ret;
    }

    const rmt_copy_encoder_config_t copy_encoder_config = {};
    ret = rmt_new_copy_encoder(&copy_encoder_config, &s_ws2812_encoder);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create RMT copy encoder: %s", esp_err_to_name(ret));
        rmt_del_channel(s_ws2812_chan);
        s_ws2812_chan = NULL;
        return ret;
    }

    ret = rmt_enable(s_ws2812_chan);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to enable RMT channel: %s", esp_err_to_name(ret));
        rmt_del_encoder(s_ws2812_encoder);
        rmt_del_channel(s_ws2812_chan);
        s_ws2812_encoder = NULL;
        s_ws2812_chan    = NULL;
        return ret;
    }

    s_ws2812_initialized = true;
    ESP_LOGI(TAG, "WS2812 module initialized on GPIO %d", WS2812_MODULE_GPIO);

    return ws2812_module_clear();
}

esp_err_t ws2812_module_deinit(void)
{
    if (!s_ws2812_initialized)
    {
        return ESP_OK;
    }

    esp_err_t ret = rmt_disable(s_ws2812_chan);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to disable RMT channel: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = rmt_del_encoder(s_ws2812_encoder);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to delete RMT encoder: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = rmt_del_channel(s_ws2812_chan);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to delete RMT channel: %s", esp_err_to_name(ret));
        return ret;
    }

    s_ws2812_encoder     = NULL;
    s_ws2812_chan        = NULL;
    s_ws2812_initialized = false;
    ESP_LOGI(TAG, "WS2812 module deinitialized");
    return ESP_OK;
}

esp_err_t ws2812_module_set_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    s_ws2812_color.red   = red;
    s_ws2812_color.green = green;
    s_ws2812_color.blue  = blue;

    const ws2812_rgb_t scaled_color =
        ws2812_module_apply_brightness(&s_ws2812_color, s_ws2812_brightness);
    return ws2812_module_transmit_color(&scaled_color);
}

esp_err_t ws2812_module_set_brightness(uint8_t brightness)
{
    s_ws2812_brightness = brightness;
    const ws2812_rgb_t scaled_color =
        ws2812_module_apply_brightness(&s_ws2812_color, s_ws2812_brightness);
    return ws2812_module_transmit_color(&scaled_color);
}

esp_err_t ws2812_module_clear(void)
{
    const ws2812_rgb_t off = {
        .red   = 0,
        .green = 0,
        .blue  = 0,
    };
    return ws2812_module_transmit_color(&off);
}

esp_err_t ws2812_module_show_breath_step(uint32_t elapsed_ms)
{
    const uint32_t period_ms         = LED_BREATH_DEFAULT_PERIOD_MS;
    const uint32_t half_period_ms    = period_ms / 2U;
    const uint32_t phase_ms          = elapsed_ms % period_ms;
    uint8_t        effect_brightness = 0;

    if (half_period_ms == 0)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    if (phase_ms < half_period_ms)
    {
        effect_brightness =
            (uint8_t)(LED_BREATH_MIN_BRIGHTNESS +
                      ((LED_BREATH_MAX_BRIGHTNESS - LED_BREATH_MIN_BRIGHTNESS) * phase_ms) /
                          half_period_ms);
    }
    else
    {
        const uint32_t falling_phase_ms = phase_ms - half_period_ms;
        effect_brightness =
            (uint8_t)(LED_BREATH_MAX_BRIGHTNESS -
                      ((LED_BREATH_MAX_BRIGHTNESS - LED_BREATH_MIN_BRIGHTNESS) * falling_phase_ms) /
                          half_period_ms);
    }

    const uint8_t final_brightness =
        ws2812_module_scale_channel(effect_brightness, s_ws2812_brightness);
    const ws2812_rgb_t scaled_color =
        ws2812_module_apply_brightness(&s_ws2812_color, final_brightness);
    return ws2812_module_transmit_color(&scaled_color);
}

bool ws2812_module_is_initialized(void)
{
    return s_ws2812_initialized;
}

#endif
