#pragma once

#include "pin_config.h"
#include "driver/spi_master.h"

/* The original base PCB: no disabled-config fallback onto SDIO pins. */
#define LCD_WIDTH 160
#define LCD_HEIGHT 60
#define LCD_SPI_HOST SPI2_HOST
#define LCD_SPI_CLOCK_SPEED_HZ 4000000
#define LCD_TRANSFER_TIMEOUT_MS 500
#define LCD_TASK_STACK_SIZE 4096
#define LCD_TASK_PRIORITY 3
