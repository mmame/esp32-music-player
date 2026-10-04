/**
 * @file sunton_esp32_2432s032.h
 * @brief Pin / orientation configuration for the Sunton ESP32-2432S032C
 *        (ESP32-WROOM-32, 3.2" 320x240 ST7789 SPI LCD, GT911 capacitive touch).
 *        Values taken from the vendor demo (LvglWidgets_Capacitive_gt911 / touch.h).
 */
#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"

#define BOARD_LCD_WIDTH                 320     /* landscape */
#define BOARD_LCD_HEIGHT                240

/* LCD on SPI2 (HSPI) */
#define BOARD_LCD_SPI_HOST              SPI2_HOST
#define BOARD_LCD_PIN_MOSI              GPIO_NUM_13
#define BOARD_LCD_PIN_MISO              GPIO_NUM_NC   /* not used (GPIO12 is a strapping pin) */
#define BOARD_LCD_PIN_SCLK              GPIO_NUM_14
#define BOARD_LCD_PIN_CS                GPIO_NUM_15
#define BOARD_LCD_PIN_DC                GPIO_NUM_2
#define BOARD_LCD_PIN_RST               GPIO_NUM_NC   /* tied to EN */
#define BOARD_LCD_PIN_BCKL              GPIO_NUM_27
#define BOARD_LCD_SPI_HZ                (40 * 1000 * 1000)

/* Panel is natively 240x320 portrait; the demo's "rotation 3" = MADCTL MV|MY (landscape) */
#define BOARD_LCD_SWAP_XY               1
#define BOARD_LCD_MIRROR_X              0
#define BOARD_LCD_MIRROR_Y              1
#define BOARD_LCD_INVERT                1     /* "IPS" panel: colour inversion on */
#define BOARD_LCD_BGR                   0

/* GT911 touch on I2C (INT not routed, address 0x5D) */
#define BOARD_TOUCH_PIN_SDA             GPIO_NUM_33
#define BOARD_TOUCH_PIN_SCL             GPIO_NUM_32
#define BOARD_TOUCH_PIN_RST             GPIO_NUM_25
#define BOARD_TOUCH_PIN_INT             GPIO_NUM_NC
/* GT911 native frame is 240x320 portrait; the vendor demo maps raw -> screen as
 *   screen_x = 320 - raw_y * (320 / y_max),  screen_y = raw_x * (240 / x_max)   */
#define BOARD_TOUCH_NATIVE_X_MAX        240
#define BOARD_TOUCH_NATIVE_Y_MAX        320

/* 1 = flash red/green/blue for ~1.3 s at boot (hardware sanity check, as in the vendor demo); off by default */
#define BOARD_LCD_BOOT_TEST             0

#define BOARD_BACKLIGHT_LEDC_TIMER      LEDC_TIMER_0
#define BOARD_BACKLIGHT_LEDC_CHANNEL    LEDC_CHANNEL_0

#define LVGL_TICK_PERIOD_MS             2
/* LVGL partial-render buffer height in lines (two DMA buffers of W*lines pixels) */
#define BOARD_LVGL_BUF_LINES            40
