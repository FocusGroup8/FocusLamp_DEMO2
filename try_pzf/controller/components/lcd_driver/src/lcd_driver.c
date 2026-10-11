/* GC9107 base LCD: logical 160x60 -> physical 60x160, RGB565.
 * One logical frame and one dynamic internal DMA buffer (19,200 bytes each).
 * No BSP, LVGL, static full-frame DMA arrays or voice/peripheral side effects. */
#include "lcd_driver.h"
#include "lcd_font.h"
#include "lcd_font_cn.h"

#include <string.h>
#include <stdio.h>
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lcd_gc9107.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define PHYS_WIDTH 60
#define PHYS_HEIGHT 160
#define FRAME_BYTES (PHYS_WIDTH * PHYS_HEIGHT * sizeof(uint16_t))
_Static_assert(LCD_WIDTH * LCD_HEIGHT == PHYS_WIDTH * PHYS_HEIGHT, "LCD geometry mismatch");
_Static_assert(LCD_BL_GPIO == 51, "The base LCD backlight must use GPIO51");

static const char *TAG = "LCD_DRIVER";
static SemaphoreHandle_t s_mutex; /* Persistent: a failed init can be retried. */
static SemaphoreHandle_t s_tx_done;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_inflight;
static bool s_fault;
static bool s_initialized;
static bool s_owns_bus;
static bool s_backlight_ready;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static lcd_color_t *s_frame_buffer;
static uint16_t *s_phys_buffer;

static TickType_t wait_ticks(void)
{
    TickType_t ticks = pdMS_TO_TICKS(LCD_TRANSFER_TIMEOUT_MS);
    return ticks ? ticks : 1;
}

/* Caller holds s_mutex. Restore the original base backlight configuration
 * for init and every control request; input enable makes pad readback useful.
 * A pad level is not proof of LCD supply, wiring or visible light. */
static esp_err_t backlight_set_locked(bool on, const char *where)
{
    esp_err_t ret = gpio_reset_pin(LCD_BL_GPIO);
    if (ret != ESP_OK) return ret;
    /* Preload output latch before enabling output to limit transition glitches. */
    ret = gpio_set_level(LCD_BL_GPIO, on ? 1 : 0);
    if (ret != ESP_OK) return ret;
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << LCD_BL_GPIO,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&config);
    if (ret != ESP_OK) return ret;
    s_backlight_ready = true;
    ret = gpio_set_level(LCD_BL_GPIO, on ? 1 : 0);
    if (ret != ESP_OK) return ret;
    int pad = gpio_get_level(LCD_BL_GPIO);
    ESP_LOGI(TAG, "[BL] %s GPIO=%d requested=%d pad_readback=%d mode=INPUT_OUTPUT pullup=1 (not a light sensor)",
             where, LCD_BL_GPIO, on ? 1 : 0, pad);
    if (pad != (on ? 1 : 0)) {
        ESP_LOGW(TAG, "[BL] Pad differs from requested level; check GPIO configuration/external load, not just software");
    }
    return ESP_OK;
}

static esp_err_t backlight_startup_test_locked(void)
{
    /* One visible test per successful init; no new task or periodic flashing.
     * It runs only AFTER the first black frame has completed. */
    ESP_LOGI(TAG, "[BL_TEST] START: HIGH 700ms -> LOW 700ms -> HIGH retained; watch the LCD backlight");
    esp_err_t ret = backlight_set_locked(true, "test HIGH");
    if (ret != ESP_OK) return ret;
    ret = gpio_dump_io_configuration(stdout, 1ULL << LCD_BL_GPIO);
    if (ret != ESP_OK) ESP_LOGW(TAG, "[BL] GPIO configuration dump failed: %s", esp_err_to_name(ret));
    vTaskDelay(pdMS_TO_TICKS(700));
    ret = backlight_set_locked(false, "test LOW");
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(700));
    ret = backlight_set_locked(true, "test HIGH final");
    if (ret == ESP_OK) ESP_LOGI(TAG, "[BL_TEST] END: backlight requested ON; physical light still needs observation");
    return ret;
}

static bool IRAM_ATTR transfer_done(esp_lcd_panel_io_handle_t io,
                                   esp_lcd_panel_io_event_data_t *event, void *ctx)
{
    (void)io;
    (void)event;
    BaseType_t wake = pdFALSE;
    portENTER_CRITICAL_ISR(&s_state_lock);
    s_inflight = false;
    portEXIT_CRITICAL_ISR(&s_state_lock);
    xSemaphoreGiveFromISR((SemaphoreHandle_t)ctx, &wake);
    /* This IDF SPI wrapper does not act on the callback's bool return. */
    if (wake == pdTRUE) portYIELD_FROM_ISR();
    return wake == pdTRUE;
}

static void memory_diag(const char *where)
{
    const uint32_t caps[] = {MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL,
                             MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL};
    for (unsigned i = 0; i < 2; ++i) {
        ESP_LOGI(TAG, "LCD memory [%s]: caps=0x%x free=%u largest=%u",
                 where, (unsigned)caps[i],
                 (unsigned)heap_caps_get_free_size(caps[i]),
                 (unsigned)heap_caps_get_largest_free_block(caps[i]));
    }
}

/* The caller holds s_mutex. The token is ownership of the physical buffer. */
static esp_err_t acquire_buffer(void)
{
    if (xSemaphoreTake(s_tx_done, wait_ticks()) != pdTRUE) {
        s_fault = true;
        ESP_LOGE(TAG, "LCD DMA wait timeout; refresh stopped, resources retained");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t submit_buffer(void)
{
    /* Set before queueing: the ISR may complete before draw_bitmap returns. */
    portENTER_CRITICAL(&s_state_lock);
    s_inflight = true;
    portEXIT_CRITICAL(&s_state_lock);
    esp_err_t ret = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, PHYS_WIDTH, PHYS_HEIGHT,
                                            s_phys_buffer);
    if (ret != ESP_OK) {
        /* Invariant checked at init: this full frame fits ONE SPI transaction.
         * CASET/RASET failures stop before color; a failed single queue operation
         * queues no color transaction. No partial multi-chunk frame is possible. */
        portENTER_CRITICAL(&s_state_lock);
        s_inflight = false;
        portEXIT_CRITICAL(&s_state_lock);
        s_fault = true;
        xSemaphoreGive(s_tx_done);
        ESP_LOGE(TAG, "LCD submit failed: %s; no color queued, refresh stopped", esp_err_to_name(ret));
        return ret;
    }
    /* Do NOT set inflight here: an already-completed ISR must stay completed. */
    ret = acquire_buffer();
    if (ret == ESP_OK) xSemaphoreGive(s_tx_done);
    return ret;
}

/* No writer remains, and the caller holds s_mutex. Never free a live DMA source. */
static esp_err_t cleanup_locked(void)
{
    s_initialized = false;
    s_fault = true;
    if (s_backlight_ready) gpio_set_level(LCD_BL_GPIO, 0);
    if (s_tx_done && xSemaphoreTake(s_tx_done, wait_ticks()) != pdTRUE) {
        ESP_LOGW(TAG, "LCD shutdown incomplete: DMA resources retained, retry deinit");
        return ESP_ERR_TIMEOUT;
    }
    if (s_panel) {
        esp_err_t ret = esp_lcd_panel_del(s_panel);
        if (ret != ESP_OK) {
            if (s_tx_done) xSemaphoreGive(s_tx_done);
            return ret;
        }
        s_panel = NULL;
    }
    if (s_io) {
        /* IO deletion drains result descriptors and finishes its callbacks. */
        esp_err_t ret = esp_lcd_panel_io_del(s_io);
        if (ret != ESP_OK) {
            if (s_tx_done) xSemaphoreGive(s_tx_done);
            return ret;
        }
        s_io = NULL;
    }
    heap_caps_free(s_phys_buffer);
    heap_caps_free(s_frame_buffer);
    s_phys_buffer = NULL;
    s_frame_buffer = NULL;
    if (s_tx_done) {
        vSemaphoreDelete(s_tx_done);
        s_tx_done = NULL;
    }
    if (s_owns_bus) {
        esp_err_t ret = spi_bus_free(LCD_SPI_HOST);
        if (ret != ESP_OK) return ret;
        s_owns_bus = false;
    }
    if (s_backlight_ready) {
        gpio_reset_pin(LCD_BL_GPIO);
        s_backlight_ready = false;
    }
    return ESP_OK;
}

esp_err_t lcd_driver_init(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(s_mutex, wait_ticks()) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (s_initialized) {
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }
    if (s_io || s_panel || s_tx_done || s_owns_bus) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE; /* Deferred cleanup must finish first. */
    }
    s_fault = false;
    esp_err_t ret;
    size_t max_bytes = 0;
    ESP_LOGI(TAG, "Initializing LCD driver (160x60 logical, 60x160 physical) BL_gpio=%d", LCD_BL_GPIO);
    memory_diag("before allocate");
    s_tx_done = xSemaphoreCreateBinary();
    if (!s_tx_done) { ret = ESP_ERR_NO_MEM; goto fail; }
    xSemaphoreGive(s_tx_done); /* First frame has no outstanding transfer. */
    s_frame_buffer = heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_phys_buffer = heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_frame_buffer || !s_phys_buffer) {
        ESP_LOGE(TAG, "Failed to allocate LCD buffers (19200 + 19200 bytes)");
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    memset(s_frame_buffer, 0, FRAME_BYTES);
    memory_diag("after allocate");

    ret = backlight_set_locked(false, "init OFF");
    if (ret != ESP_OK) goto fail;

    spi_bus_config_t bus = {
        .sclk_io_num = LCD_SCL_GPIO, .mosi_io_num = LCD_SDA_GPIO, .miso_io_num = -1,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = FRAME_BYTES,
    };
    ESP_LOGI(TAG, "[SPI_INIT] bus=%d mosi=%d sclk=%d cs=%d dc=%d speed=%d",
             LCD_SPI_HOST, LCD_SDA_GPIO, LCD_SCL_GPIO, LCD_CS_GPIO, LCD_DC_GPIO, LCD_SPI_CLOCK_SPEED_HZ);
    ret = spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) goto fail; /* Never reuse or free another owner's bus. */
    s_owns_bus = true;
    esp_lcd_panel_io_spi_config_t io = {
        .dc_gpio_num = LCD_DC_GPIO, .cs_gpio_num = LCD_CS_GPIO,
        .pclk_hz = LCD_SPI_CLOCK_SPEED_HZ, .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .spi_mode = 0, .trans_queue_depth = 10,
        .on_color_trans_done = transfer_done, .user_ctx = s_tx_done,
    };
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io, &s_io);
    if (ret != ESP_OK) goto fail;
    /* SPI master context is lazily created by the first add_device inside
     * new_panel_io_spi, not by bus_initialize. Keep the one-transaction proof. */
    ret = spi_bus_get_max_transaction_len(LCD_SPI_HOST, &max_bytes);
    if (ret != ESP_OK || max_bytes < FRAME_BYTES) {
        ESP_LOGE(TAG, "LCD transaction-size check failed: query=%s max_bytes=%u frame_bytes=%u",
                 esp_err_to_name(ret), (unsigned)max_bytes, (unsigned)FRAME_BYTES);
        if (ret == ESP_OK) ret = ESP_ERR_NOT_SUPPORTED;
        goto fail;
    }
    ESP_LOGI(TAG, "LCD single-transaction check passed: max_bytes=%u frame_bytes=%u",
             (unsigned)max_bytes, (unsigned)FRAME_BYTES);
    esp_lcd_panel_dev_config_t panel = {
        .reset_gpio_num = -1, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16,
    };
    ret = esp_lcd_new_panel_gc9107(s_io, &panel, &s_panel);
    if (ret != ESP_OK) goto fail;
    ret = esp_lcd_panel_reset(s_panel);
    if (ret != ESP_OK) goto fail;
    ret = esp_lcd_panel_init(s_panel);
    if (ret != ESP_OK) goto fail;
    ret = esp_lcd_panel_set_gap(s_panel, 34, 0);
    if (ret != ESP_OK) goto fail;
    ret = esp_lcd_panel_disp_on_off(s_panel, true);
    if (ret != ESP_OK) goto fail;

    ret = acquire_buffer();
    if (ret != ESP_OK) goto fail;
    memset(s_phys_buffer, 0, FRAME_BYTES);
    ret = submit_buffer(); /* Completion, not submission, precedes backlight. */
    if (ret != ESP_OK) goto fail;
    ret = backlight_startup_test_locked();
    if (ret != ESP_OK) goto fail;
    s_initialized = true;
    ESP_LOGI(TAG, "LCD driver initialized successfully: first black frame completed, buffers=38400 B");
    xSemaphoreGive(s_mutex);
    return ESP_OK;
fail:
    ESP_LOGW(TAG, "LCD init failed: %s (other modules continue)", esp_err_to_name(ret));
    {
        esp_err_t cleanup_ret = cleanup_locked();
        if (cleanup_ret != ESP_OK) ESP_LOGW(TAG, "LCD cleanup deferred: %s", esp_err_to_name(cleanup_ret));
    }
    xSemaphoreGive(s_mutex);
    return ret;
}

esp_err_t lcd_driver_deinit(void)
{
    if (!s_mutex) return ESP_OK;
    if (xSemaphoreTake(s_mutex, wait_ticks()) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t ret = cleanup_locked();
    xSemaphoreGive(s_mutex);
    return ret;
}

static esp_err_t set_display(bool on)
{
    if (!s_mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mutex, wait_ticks()) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (!s_initialized || s_fault) goto out;
    if (on) {
        ret = esp_lcd_panel_disp_on_off(s_panel, true);
        if (ret == ESP_OK) ret = backlight_set_locked(true, "display ON");
    } else {
        ret = backlight_set_locked(false, "display OFF");
        if (ret == ESP_OK) ret = esp_lcd_panel_disp_on_off(s_panel, false);
    }
    if (ret == ESP_OK) ESP_LOGI(TAG, "Local display %s (digital backlight %s)",
                              on ? "on" : "off", on ? "on" : "off");
out:
    xSemaphoreGive(s_mutex);
    return ret;
}

esp_err_t lcd_driver_display_on(void) { return set_display(true); }
esp_err_t lcd_driver_display_off(void) { return set_display(false); }

esp_err_t lcd_driver_set_backlight(uint8_t brightness)
{
    if (!s_mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mutex, wait_ticks()) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (s_initialized && !s_fault) {
        ret = backlight_set_locked(brightness > 0, "set backlight");
        if (ret == ESP_OK) ESP_LOGI(TAG, "Local backlight: requested=%u digital=%u (not PWM)",
                                  (unsigned)brightness, brightness > 0 ? 1U : 0U);
    }
    xSemaphoreGive(s_mutex);
    return ret;
}

esp_err_t lcd_flush_buffer(void)
{
    if (!s_mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mutex, wait_ticks()) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (!s_initialized || s_fault) goto out;
    ret = acquire_buffer();
    if (ret != ESP_OK) goto out;
    /* The previous completion token was acquired BEFORE any physical write. */
    for (int y = 0; y < LCD_HEIGHT; ++y) {
        for (int x = 0; x < LCD_WIDTH; ++x) {
            int px = LCD_HEIGHT - 1 - y;
            int py = x;
            s_phys_buffer[py * PHYS_WIDTH + px] = __builtin_bswap16(s_frame_buffer[y * LCD_WIDTH + x]);
        }
    }
    ret = submit_buffer();
out:
    xSemaphoreGive(s_mutex);
    return ret;
}

/* Logical-frame primitives adapted from the base lcd_driver. The UI is the
 * only writer, and deinit is called only after that writer has stopped. */
void lcd_fill_rect_buffer(uint16_t x, uint16_t y, uint16_t w, uint16_t h, lcd_color_t color)
{
    if (!s_initialized || !s_frame_buffer || x >= LCD_WIDTH || y >= LCD_HEIGHT) return;
    if (w > LCD_WIDTH - x) w = LCD_WIDTH - x;
    if (h > LCD_HEIGHT - y) h = LCD_HEIGHT - y;
    for (uint16_t j = 0; j < h; ++j)
        for (uint16_t i = 0; i < w; ++i) s_frame_buffer[(y + j) * LCD_WIDTH + x + i] = color;
}

void lcd_draw_char_buffer(uint16_t x, uint16_t y, char c, lcd_color_t color, lcd_color_t bg, uint8_t size)
{
    if (!s_initialized || !s_frame_buffer || size == 0) return;
    if (c < ' ' || c > '~') c = '?';
    c -= ' ';
    for (uint8_t row = 0; row < 12; ++row) {
        uint8_t bits = ascii_1206[(uint8_t)c][row];
        for (uint8_t col = 0; col < 8; ++col) {
            for (uint8_t dy = 0; dy < size; ++dy) {
                for (uint8_t dx = 0; dx < size; ++dx) {
                    uint32_t px = (uint32_t)x + col * size + dx;
                    uint32_t py = (uint32_t)y + row * size + dy;
                    if (px < LCD_WIDTH && py < LCD_HEIGHT)
                        s_frame_buffer[py * LCD_WIDTH + px] = (bits & 1) ? color : bg;
                }
            }
            bits >>= 1;
        }
    }
}

void lcd_draw_string_buffer(uint16_t x, uint16_t y, const char *text,
                            lcd_color_t color, lcd_color_t bg, uint8_t size)
{
    if (!text) return;
    uint16_t x0 = x;
    while (*text && x < LCD_WIDTH && y < LCD_HEIGHT) {
        if (*text == '\n') { x = x0; y += 12 * size; }
        else { lcd_draw_char_buffer(x, y, *text, color, bg, size); x += 8 * size; }
        ++text;
    }
}

void lcd_draw_utf8_string_buffer(uint16_t x, uint16_t y, const char *text,
                                 lcd_color_t color, lcd_color_t bg, uint8_t size)
{
    if (!s_initialized || !s_frame_buffer || !text) return;
    while (*text && x < LCD_WIDTH) {
        uint8_t b = (uint8_t)*text;
        if (b >= 0xe0 && b <= 0xef && text[1] && text[2]) {
            uint32_t key = ((uint32_t)b << 16) | ((uint32_t)(uint8_t)text[1] << 8) | (uint8_t)text[2];
            const uint16_t *bitmap = lcd_cn_lookup(key);
            if (bitmap) {
                for (unsigned row = 0; row < 16; ++row)
                    for (unsigned col = 0; col < 16; ++col)
                        if (x + col < LCD_WIDTH && y + row < LCD_HEIGHT)
                            s_frame_buffer[(y + row) * LCD_WIDTH + x + col] =
                                (bitmap[row] & (1U << (15 - col))) ? color : bg;
            }
            x += 16; text += 3;
        } else if (b >= 0x80) ++text;
        else { lcd_draw_char_buffer(x, y, *text++, color, bg, size); x += 8 * size; }
    }
}
