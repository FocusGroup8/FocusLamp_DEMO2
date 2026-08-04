/*
 * bsp_gpio.c - GPIO initialization and control for FocusLamp BSP
 */

#include "bsp_gpio.h"
#include "pin_config.h"

/* All output GPIOs used by the BSP layer */
/* Note: LED_DIN_GPIO (WS2812 data) is NOT included here - it's driven by RMT
 * peripheral, not GPIO. Pre-configuring it as GPIO output can conflict with RMT. */
static const gpio_num_t s_output_gpios[] = {
    ESP_EN_GPIO,        /* ESP enable */
    LCD_BL_GPIO,        /* LCD backlight */
    LCD_DC_GPIO,        /* LCD data/command */
    LCD_CS_GPIO,        /* LCD chip select */
    SERVO_OE1_GPIO,     /* Servo 1 output enable */
    SERVO_OE2_GPIO,     /* Servo 2 output enable */
};

#define BSP_GPIO_OUTPUT_COUNT   (sizeof(s_output_gpios) / sizeof(s_output_gpios[0]))

/* Default output levels (initially inactive/low where safe) */
static const uint32_t s_default_levels[] = {
    0,  /* ESP_EN   - ESP disabled by default */
    0,  /* LCD_BL   - backlight off by default */
    0,  /* LCD_DC   - command mode by default */
    1,  /* LCD_CS   - deselect (CS active low assumed) */
    0,  /* SERVO_OE1 - servo 1 disabled */
    0,  /* SERVO_OE2 - servo 2 disabled */
};

esp_err_t bsp_gpio_init(void)
{
    esp_err_t ret;

    for (size_t i = 0; i < BSP_GPIO_OUTPUT_COUNT; i++) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << s_output_gpios[i]),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ret = gpio_config(&io_conf);
        if (ret != ESP_OK) {
            return ret;
        }

        ret = gpio_set_level(s_output_gpios[i], s_default_levels[i]);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    return ESP_OK;
}

esp_err_t bsp_gpio_set_level(gpio_num_t gpio, uint32_t level)
{
    /* TODO: Add validation for allowed GPIOs if needed */
    return gpio_set_level(gpio, level);
}

int bsp_gpio_get_level(gpio_num_t gpio)
{
    /* TODO: Add validation for allowed GPIOs if needed */
    return gpio_get_level(gpio);
}