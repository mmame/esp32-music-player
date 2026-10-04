/**
 * @file sunton_esp32_8048s050c.h
 * @author Sven Fabricius (sven.fabricius@livediesel.de)
 * @brief
 * @version 0.1
 * @date 2024-09-09
 *
 * @copyright Copyright (c) 2024
 *
 */
#pragma once

#define SUNTON_ESP32_LCD_WIDTH                  800
#define SUNTON_ESP32_LCD_HEIGHT                 480

#define SUNTON_ESP32_PIN_BCKL                   GPIO_NUM_2

// GT911 Pin config
#define SUNTON_ESP32_TOUCH_PIN_I2C_SCL          GPIO_NUM_20
#define SUNTON_ESP32_TOUCH_PIN_I2C_SDA          GPIO_NUM_19
#define SUNTON_ESP32_TOUCH_PIN_RST              GPIO_NUM_38
// interupt pin was falsely routed to GND instead via R17 to IO18
#define SUNTON_ESP32_TOUCH_PIN_INT              GPIO_NUM_NC

// interupt pin was falsely routed to GND, so its 0x5D
#define SUNTON_ESP32_TOUCH_ADDRESS              ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS

// 1 = show GT911 config/raw/mapped touch info and a red dot at the touch point on screen
#define SUNTON_ESP32_TOUCH_DEBUG                0

// not required, external pullups R3 / R4 in place
//#define SUNTON_ESP32_TOUCH_I2C_PULLUP           y

#define SUNTON_ESP32_BACKLIGHT_LEDC_TIMER       LEDC_TIMER_0
#define SUNTON_ESP32_BACKLIGHT_LEDC_CHANNEL     LEDC_CHANNEL_0

#define LVGL_TICK_PERIOD_MS                     2

void board_backlight_init(void);
lv_display_t *board_lcd_init(void);
i2c_master_bus_handle_t board_i2c_master(void);
void board_touch_init(i2c_master_bus_handle_t i2c_master);
