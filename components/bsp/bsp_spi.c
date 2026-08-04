/*
 * bsp_spi.c - SPI initialization and communication for FocusLamp BSP
 */

#include "bsp_spi.h"
#include "pin_config.h"

/* SPI bus and device handles */
static spi_device_handle_t s_spi_handle = NULL;

/* SPI configuration */
#define BSP_SPI_HOST            SPI2_HOST
#define BSP_SPI_CLOCK_SPEED_HZ  (4 * 1000 * 1000)   /* 4 MHz for GC9107 stability during full-screen writes */
#define BSP_SPI_QUEUE_SIZE      7

esp_err_t bsp_spi_init(void)
{
    esp_err_t ret;

    /* Bus configuration */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = LCD_SDA_GPIO,      /* GPIO49 */
        .miso_io_num     = -1,                 /* LCD is write-only */
        .sclk_io_num     = LCD_SCL_GPIO,       /* GPIO36 */
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4096,
    };

    ret = spi_bus_initialize(BSP_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) return ret;

    /* Device configuration */
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = BSP_SPI_CLOCK_SPEED_HZ,
        .mode           = 3,                   /* Align with FocusLamp validated GC9107 SPI mode */
        .spics_io_num   = -1,                  /* CS controlled manually by driver */
        .queue_size     = BSP_SPI_QUEUE_SIZE,
        .flags          = SPI_DEVICE_HALFDUPLEX,
        /* TODO: Set .pre_cb / .post_cb if DC GPIO control via callback is desired;
         *       otherwise DC should be toggled by the caller. */
    };

    ret = spi_bus_add_device(BSP_SPI_HOST, &dev_cfg, &s_spi_handle);
    if (ret != ESP_OK) return ret;

    return ESP_OK;
}

esp_err_t bsp_spi_transmit_bytes(const uint8_t *data, size_t len)
{
    if (s_spi_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    spi_transaction_t trans = {
        .length    = len * 8,     /* Length in bits */
        .tx_buffer = data,
    };

    /* TODO: The caller is responsible for controlling LCD_DC (data/command) GPIO
     *       before calling this function. A DC callback could be added to
     *       spi_device_interface_config_t for automatic control. */

    return spi_device_transmit(s_spi_handle, &trans);
}

esp_err_t bsp_spi_transmit_byte(uint8_t byte)
{
    return bsp_spi_transmit_bytes(&byte, 1);
}