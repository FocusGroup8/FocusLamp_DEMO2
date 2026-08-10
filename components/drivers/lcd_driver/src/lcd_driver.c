/*
 * lcd_driver.c - LCD display driver implementation
 *
 * Uses ESP-IDF esp_lcd API with esp_lcd_gc9107 component.
 * Frame buffer support with software rotation (160x60 landscape -> 60x160 portrait).
 */

#include "lcd_driver.h"
#include "lcd_font.h"
#include "lcd_font_cn.h"
#include "pin_config.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_gc9107.h"

static const char *TAG = "LCD_DRIVER";

// #region forward-declarations
static void lcd_lock(void);
static void lcd_unlock(void);
// #endregion

/* Physical panel dimensions (portrait mode, before software rotation) */
#define LCD_PHYS_WIDTH             60
#define LCD_PHYS_HEIGHT            160

/* Frame buffer for pixel operations */
static lcd_color_t *s_frame_buffer = NULL;
static bool s_driver_initialized = false;
static SemaphoreHandle_t s_lcd_mutex = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_panel_io_handle_t s_io = NULL;

/* Backlight state (GPIO control, LEDC not used on ESP32-P4) */
static bool s_pwm_initialized = false;

static void lcd_fill_screen_raw(lcd_color_t color)
{
    /* Double-buffered DMA fill buffer: while one buffer is being transferred
     * by the async SPI DMA, the next fill writes to the other one. Reusing a
     * single static buffer could let a later write overwrite data the DMA has
     * not yet read, producing corrupted/white frames. */
    static DMA_ATTR uint16_t s_phys_fill_buf[2][LCD_PHYS_WIDTH * LCD_PHYS_HEIGHT];
    static uint8_t s_fill_buf_idx = 0;
    uint16_t *buf = s_phys_fill_buf[s_fill_buf_idx];
    s_fill_buf_idx ^= 1;

    /* Build a physical-sized buffer filled with the color (byte-swapped for
     * GC9107). DMA_ATTR keeps the buffer in internal RAM (not PSRAM) so SPI
     * DMA can access it reliably. Filling happens inside the lock so two
     * tasks cannot race on the same buffer. */
    uint16_t swapped = (color >> 8) | (color << 8);
    lcd_lock();
    for (int i = 0; i < LCD_PHYS_WIDTH * LCD_PHYS_HEIGHT; i++) {
        buf[i] = swapped;
    }
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_PHYS_WIDTH, LCD_PHYS_HEIGHT, buf);
    lcd_unlock();
}


/* GC9107 panel settings (1.45" TFT, portrait mode):
 * - MADCTL=0x00 -> portrait orientation
 * - GRAM is 128 columns but only 60 visible, X needs +34 offset.
 * - Physical panel: 60 (width) x 160 (height) portrait.
 * - Logical frame buffer: 160 (LCD_WIDTH) x 60 (LCD_HEIGHT) landscape.
 * - Software rotation in lcd_flush_buffer() maps logical -> physical.
 */
#define LCD_X_OFFSET              34
#define LCD_Y_OFFSET              0
#define LCD_MADCTL_VALUE          0x00

/* Backlight polarity: 1 = Active Low, 0 = Active High */
#define LCD_BL_ACTIVE_LOW         0

static inline void lcd_lock(void)
{
    if (s_lcd_mutex != NULL) xSemaphoreTakeRecursive(s_lcd_mutex, portMAX_DELAY);
}

static inline void lcd_unlock(void)
{
    if (s_lcd_mutex != NULL) xSemaphoreGiveRecursive(s_lcd_mutex);
}

static esp_err_t lcd_spi_init(void)
{
    ESP_LOGI(TAG, "[SPI_INIT] bus=%d mosi=%d sclk=%d cs=%d dc=%d speed=%lu",
             LCD_SPI_HOST,
             LCD_SDA_GPIO, LCD_SCL_GPIO, LCD_CS_GPIO, LCD_DC_GPIO,
             (unsigned long)LCD_SPI_CLOCK_SPEED_HZ);

    /* Initialize SPI bus */
    spi_bus_config_t buscfg = {
        .sclk_io_num = LCD_SCL_GPIO,
        .mosi_io_num = LCD_SDA_GPIO,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_PHYS_WIDTH * LCD_PHYS_HEIGHT * sizeof(uint16_t),
    };
    esp_err_t ret = spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Create panel IO (handles DC/CS automatically) */
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_DC_GPIO,
        .cs_gpio_num = LCD_CS_GPIO,
        .pclk_hz = LCD_SPI_CLOCK_SPEED_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_config, &s_io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "[SPI_INIT] panel_io created, result=%s", esp_err_to_name(ret));
    return ESP_OK;
}

/**
 * @brief Initialize backlight GPIO on LCD_BL_GPIO.
 */
static esp_err_t lcd_pwm_init(void)
{
    if (s_pwm_initialized) {
        return ESP_OK;
    }

    gpio_reset_pin(LCD_BL_GPIO);
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LCD_BL_GPIO),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure backlight GPIO: %s", esp_err_to_name(ret));
        return ret;
    }

    s_pwm_initialized = true;
    ESP_LOGI(TAG, "Backlight GPIO initialized (GPIO=%d)", LCD_BL_GPIO);
    return ESP_OK;
}

esp_err_t lcd_driver_init(void)
{
    esp_err_t ret;

    if (s_driver_initialized) {
        ESP_LOGW(TAG, "LCD driver already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing LCD driver (%dx%d logical, %dx%d physical) BL_gpio=%d",
             LCD_WIDTH, LCD_HEIGHT, LCD_PHYS_WIDTH, LCD_PHYS_HEIGHT, LCD_BL_GPIO);

    if (s_lcd_mutex == NULL) {
        s_lcd_mutex = xSemaphoreCreateRecursiveMutex();
        if (s_lcd_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create LCD mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG, "[INIT] Step1: SPI + panel IO init");
    ret = lcd_spi_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize LCD SPI: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "[INIT] Step2: GC9107 panel create + init");
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ret = esp_lcd_new_panel_gc9107(s_io, &panel_config, &s_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_gc9107 failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, LCD_X_OFFSET, LCD_Y_OFFSET));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    ESP_LOGI(TAG, "[INIT] Step3: Backlight GPIO init");
    ret = lcd_pwm_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Backlight init failed: %s", esp_err_to_name(ret));
    }

    /* Allocate frame buffer (logical dimensions) in internal DMA-capable RAM.
     * With PSRAM enabled, plain malloc() may place the buffer in PSRAM which
     * can cause SPI DMA transfer issues on some chips. */
    s_frame_buffer = (lcd_color_t *)heap_caps_malloc(LCD_WIDTH * LCD_HEIGHT * sizeof(lcd_color_t),
                                                      MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s_frame_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate frame buffer (%lu bytes)",
                 (unsigned long)(LCD_WIDTH * LCD_HEIGHT * sizeof(lcd_color_t)));
        lcd_driver_display_off();
        lcd_driver_set_backlight(0);
        return ESP_ERR_NO_MEM;
    }
    memset(s_frame_buffer, 0, LCD_WIDTH * LCD_HEIGHT * sizeof(lcd_color_t));
    s_driver_initialized = true;

    /* Clear the panel to black BEFORE enabling the backlight. GC9107's GRAM
     * power-on default may be all-ones (white); showing that content while
     * the backlight is already on is a source of the probabilistic white
     * screen at boot. */
    lcd_fill_screen_raw(0);

    ESP_LOGI(TAG, "[INIT] Setting backlight ON (brightness=255)");
    lcd_driver_set_backlight(255);
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "LCD driver initialized successfully");
    return ESP_OK;
}

esp_err_t lcd_driver_deinit(void)
{
    if (!s_driver_initialized) {
        ESP_LOGW(TAG, "LCD driver not initialized");
        return ESP_OK;
    }

    if (s_frame_buffer != NULL) {
        free(s_frame_buffer);
        s_frame_buffer = NULL;
    }

    if (s_panel != NULL) {
        esp_lcd_panel_del(s_panel);
        s_panel = NULL;
    }
    if (s_io != NULL) {
        esp_lcd_panel_io_del(s_io);
        s_io = NULL;
        spi_bus_free(LCD_SPI_HOST);
    }
    s_driver_initialized = false;
    if (s_lcd_mutex != NULL) {
        vSemaphoreDelete(s_lcd_mutex);
        s_lcd_mutex = NULL;
    }
    ESP_LOGI(TAG, "LCD driver deinitialized");
    return ESP_OK;
}

void lcd_driver_write_command(uint8_t cmd)
{
    if (s_io == NULL) return;
    lcd_lock();
    esp_lcd_panel_io_tx_param(s_io, cmd, NULL, 0);
    lcd_unlock();
}

void lcd_driver_write_data(uint8_t *data, uint16_t len)
{
    if (s_io == NULL || data == NULL || len == 0) return;
    lcd_lock();
    esp_lcd_panel_io_tx_param(s_io, 0x00, data, len);
    lcd_unlock();
}

void lcd_driver_set_area(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    /* No-op: esp_lcd_panel_draw_bitmap handles windowing internally */
    (void)x1; (void)y1; (void)x2; (void)y2;
}

esp_err_t lcd_driver_display_on(void)
{
    if (s_panel) {
        return esp_lcd_panel_disp_on_off(s_panel, true);
    }
    return ESP_ERR_INVALID_STATE;
}

esp_err_t lcd_driver_display_off(void)
{
    if (s_panel) {
        return esp_lcd_panel_disp_on_off(s_panel, false);
    }
    return ESP_ERR_INVALID_STATE;
}

void lcd_driver_set_backlight(uint8_t brightness)
{
    if (!s_pwm_initialized) {
        return;
    }

    /*
     * Use direct GPIO control (LEDC PWM does not work on GPIO51 on ESP32-P4).
     * For active_high: brightness>0 → HIGH, brightness=0 → LOW
     * For active_low:  brightness>0 → LOW,  brightness=0 → HIGH
     */
    int level = LCD_BL_ACTIVE_LOW ? (brightness == 0) : (brightness > 0);

    /* Re-configure GPIO to ensure it's still in the right state (something may have changed it) */
    gpio_reset_pin(LCD_BL_GPIO);
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LCD_BL_GPIO),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    gpio_set_level(LCD_BL_GPIO, level);
}

/* ===================== Frame Buffer Drawing Functions ===================== */

void lcd_fill_screen(lcd_color_t color)
{
    if (!s_driver_initialized) {
        return;
    }

    lcd_fill_screen_raw(color);
}

void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, lcd_color_t color)
{
    /* Direct SPI rect fill - not supported with esp_lcd API in logical coords.
     * Use frame buffer functions + lcd_flush_buffer instead. */
    lcd_fill_rect_buffer(x, y, w, h, color);
}

void lcd_draw_hline_buffer(uint16_t x, uint16_t y, uint16_t width, lcd_color_t color)
{
    if (!s_driver_initialized || s_frame_buffer == NULL) {
        return;
    }

    if (y >= LCD_HEIGHT || x >= LCD_WIDTH) {
        return;
    }

    if (x + width > LCD_WIDTH) {
        width = LCD_WIDTH - x;
    }

    for (uint16_t i = 0; i < width; i++) {
        s_frame_buffer[y * LCD_WIDTH + x + i] = color;
    }
}

void lcd_fill_rect_buffer(uint16_t x, uint16_t y, uint16_t w, uint16_t h, lcd_color_t color)
{
    if (!s_driver_initialized || s_frame_buffer == NULL) {
        return;
    }

    if (x >= LCD_WIDTH || y >= LCD_HEIGHT) {
        return;
    }

    if (x + w > LCD_WIDTH) {
        w = LCD_WIDTH - x;
    }

    if (y + h > LCD_HEIGHT) {
        h = LCD_HEIGHT - y;
    }

    for (uint16_t j = 0; j < h; j++) {
        for (uint16_t i = 0; i < w; i++) {
            s_frame_buffer[(y + j) * LCD_WIDTH + (x + i)] = color;
        }
    }
}

void lcd_flush_buffer(void)
{
    if (!s_driver_initialized || s_frame_buffer == NULL) {
        return;
    }

    /* Double-buffered DMA buffer: while one buffer is being transferred by
     * the async SPI DMA, the next frame writes to the other one. Reusing a
     * single static buffer let a later frame overwrite data the DMA had not
     * yet read, which caused random corrupted / white frames (the lcd_task
     * and EV_LCD_UPDATE event handler refresh concurrently). */
    static DMA_ATTR uint16_t s_physbuf[2][LCD_PHYS_WIDTH * LCD_PHYS_HEIGHT];
    static uint8_t s_physbuf_idx = 0;
    uint16_t *buf = s_physbuf[s_physbuf_idx];
    s_physbuf_idx ^= 1;

    /* DMA_ATTR keeps the buffer in internal RAM (not PSRAM) so SPI DMA can
     * access it reliably. The rotation + fill + draw_bitmap all happen inside
     * the lock so two tasks cannot race on the same buffer. */
    lcd_lock();
    if (LCD_WIDTH == LCD_PHYS_WIDTH && LCD_HEIGHT == LCD_PHYS_HEIGHT) {
        /* Portrait mode: direct copy with byte swap */
        for (int i = 0; i < LCD_PHYS_WIDTH * LCD_PHYS_HEIGHT; i++) {
            buf[i] = __builtin_bswap16(s_frame_buffer[i]);
        }
    } else {
        /* Landscape mode: software rotation logical (WxH) -> physical (HxW)
         * Maps logical (lx, ly) -> physical (px, py) = (LCD_HEIGHT-1-ly, lx) */
        for (int ly = 0; ly < LCD_HEIGHT; ly++) {
            for (int lx = 0; lx < LCD_WIDTH; lx++) {
                int px = LCD_HEIGHT - 1 - ly;
                int py = lx;
                buf[py * LCD_PHYS_WIDTH + px] = __builtin_bswap16(s_frame_buffer[ly * LCD_WIDTH + lx]);
            }
        }
    }

    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_PHYS_WIDTH, LCD_PHYS_HEIGHT, buf);
    lcd_unlock();
}

void lcd_draw_char(uint16_t x, uint16_t y, char c, lcd_color_t color, lcd_color_t bg, uint8_t size)
{
    /* Direct SPI char draw - delegate to frame buffer version */
    lcd_draw_char_buffer(x, y, c, color, bg, size);
}

void lcd_draw_char_buffer(uint16_t x, uint16_t y, char c, lcd_color_t color, lcd_color_t bg, uint8_t size)
{
    /* Bug 1: Correctly check for NULL and return early */
    if (s_frame_buffer == NULL) {
        return;
    }

    if (c < ' ' || c > '~') {
        c = '?';
    }

    c -= ' ';

    for (uint8_t i = 0; i < 12; i++) {
        uint8_t line = ascii_1206[(uint8_t)c][i];
        for (uint8_t j = 0; j < 8; j++) {
            if (line & 0x01) {
                for (uint8_t k = 0; k < size; k++) {
                    for (uint8_t l = 0; l < size; l++) {
                        uint16_t px = x + j * size + k;
                        uint16_t py = y + i * size + l;
                        if (px < LCD_WIDTH && py < LCD_HEIGHT) {
                            s_frame_buffer[py * LCD_WIDTH + px] = color;
                        }
                    }
                }
            } else if (bg != color) {
                for (uint8_t k = 0; k < size; k++) {
                    for (uint8_t l = 0; l < size; l++) {
                        uint16_t px = x + j * size + k;
                        uint16_t py = y + i * size + l;
                        if (px < LCD_WIDTH && py < LCD_HEIGHT) {
                            s_frame_buffer[py * LCD_WIDTH + px] = bg;
                        }
                    }
                }
            }
            line >>= 1;
        }
    }
}

void lcd_draw_string_buffer(uint16_t x, uint16_t y, const char *str, lcd_color_t color, lcd_color_t bg, uint8_t size)
{
    if (str == NULL || s_frame_buffer == NULL) {
        return;
    }

    uint16_t x0 = x;
    while (*str) {
        if (*str == '\n') {
            y += 12 * size;
            x = x0;
        } else {
            lcd_draw_char_buffer(x, y, *str, color, bg, size);
            x += 8 * size;
        }
        str++;
    }
}

void lcd_draw_cn_char_buffer(uint16_t x, uint16_t y, const uint16_t *bitmap,
                              lcd_color_t color, lcd_color_t bg)
{
    if (s_frame_buffer == NULL || bitmap == NULL) {
        return;
    }

    for (int row = 0; row < 16; row++) {
        uint16_t bits = bitmap[row];
        for (int col = 0; col < 16; col++) {
            uint16_t px = x + col;
            uint16_t py = y + row;
            if (px < LCD_WIDTH && py < LCD_HEIGHT) {
                s_frame_buffer[py * LCD_WIDTH + px] = (bits & (1 << (15 - col))) ? color : bg;
            }
        }
    }
}

void lcd_draw_utf8_string_buffer(uint16_t x, uint16_t y, const char *str,
                                  lcd_color_t color, lcd_color_t bg, uint8_t ascii_size)
{
    if (str == NULL || s_frame_buffer == NULL) {
        return;
    }

    uint16_t cx = x;
    while (*str) {
        uint8_t b0 = (uint8_t)*str;

        if (b0 >= 0xE0 && b0 <= 0xEF && str[1] && str[2]) {
            /* UTF-8 3-byte Chinese character */
            uint32_t key = ((uint32_t)(uint8_t)str[0] << 16) |
                           ((uint32_t)(uint8_t)str[1] << 8) |
                           (uint32_t)(uint8_t)str[2];
            const uint16_t *bmp = lcd_cn_lookup(key);
            if (bmp) {
                lcd_draw_cn_char_buffer(cx, y, bmp, color, bg);
            }
            cx += 16;
            str += 3;
        } else if (b0 >= 0x80) {
            /* Non-UTF8 or unsupported multibyte, skip 1 byte */
            str++;
        } else {
            /* ASCII */
            lcd_draw_char_buffer(cx, y, (char)b0, color, bg, ascii_size);
            cx += 8 * ascii_size;
            str++;
        }
    }
}

void lcd_draw_string(uint16_t x, uint16_t y, const char *str, lcd_color_t color, lcd_color_t bg, uint8_t size)
{
    if (!s_driver_initialized || str == NULL) {
        return;
    }

    uint16_t x0 = x;
    while (*str) {
        if (*str == '\n') {
            y += 12 * size;
            x = x0;
        } else {
            lcd_draw_char(x, y, *str, color, bg, size);
            x += 8 * size;
        }
        str++;
    }
}

esp_err_t lcd_set_brightness(uint8_t brightness)
{
    if (brightness > 100) {
        brightness = 100;
    }
    /* Convert percent (0-100) to 0-255 and forward to driver */
    lcd_driver_set_backlight((uint8_t)((uint32_t)brightness * 255 / 100));
    return ESP_OK;
}

void lcd_driver_flush_area(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2,
                           const uint8_t *color_data, size_t len)
{
    if (!s_driver_initialized || s_panel == NULL || color_data == NULL || len == 0) {
        return;
    }
    lcd_lock();
    esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2 + 1, y2 + 1, color_data);
    lcd_unlock();
}

void lcd_driver_diag_backlight(const char *from)
{
    int gpio_level = gpio_get_level(LCD_BL_GPIO);
    ESP_LOGI(TAG, "[DIAG][BL] from=%s GPIO=%d level=%d bl_init=%d drv_init=%d",
             from, LCD_BL_GPIO, gpio_level, s_pwm_initialized, s_driver_initialized);
    
    ESP_LOGI(TAG, "[DIAG] Testing BL GPIO toggle...");
    gpio_set_direction(LCD_BL_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_BL_GPIO, 1);
    ESP_LOGI(TAG, "[DIAG] BL GPIO HIGH");
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(LCD_BL_GPIO, 0);
    ESP_LOGI(TAG, "[DIAG] BL GPIO LOW");
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(LCD_BL_GPIO, 1);
    ESP_LOGI(TAG, "[DIAG] BL GPIO HIGH again");
    /* Restore backlight to configured brightness */
    lcd_driver_set_backlight(255);
}

void lcd_driver_diag_spi(const char *from)
{
    ESP_LOGI(TAG, "[DIAG][SPI] from=%s panel=%p io=%p initialized=%d",
             from, (void*)s_panel, (void*)s_io, s_driver_initialized);
}
