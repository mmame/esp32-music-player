/**
 * @file display_board.h
 * @brief Board abstraction. The implementation is chosen by CONFIG_DISPLAY_BOARD_*:
 *          8048S050C -> sunton_esp32_8048s050c.c (ESP32-S3, 800x480 RGB panel, GT911)
 *          2432S032  -> sunton_esp32_2432s032.c   (ESP32, 320x240 SPI ST7789, GT911)
 *        The UI layout follows the same choice (see ui_layout.h).
 */
#pragma once

#include "freertos/FreeRTOS.h"
#include "driver/i2c_master.h"
#include "lvgl.h"

void board_backlight_init(void);
/** Creates the panel + LVGL display (and the LVGL task); returns the display. */
lv_display_t *board_lcd_init(void);
i2c_master_bus_handle_t board_i2c_master(void);
/** Creates the LVGL pointer input device for the touch controller. */
void board_touch_init(i2c_master_bus_handle_t i2c_master);
