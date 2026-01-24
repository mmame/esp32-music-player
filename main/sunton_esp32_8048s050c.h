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

#include "driver/i2c_master.h"

#define SUNTON_ESP32_LCD_WIDTH                  800
#define SUNTON_ESP32_LCD_HEIGHT                 480

// Backlight is now hardwired to VCC - GPIO2 available for other uses
// #define SUNTON_ESP32_PIN_BCKL                   GPIO_NUM_2

// GT911 Pin config
#define SUNTON_ESP32_TOUCH_PIN_I2C_SCL          GPIO_NUM_20
#define SUNTON_ESP32_TOUCH_PIN_I2C_SDA          GPIO_NUM_19
#define SUNTON_ESP32_TOUCH_PIN_RST              GPIO_NUM_38
// interupt pin was falsely routed to GND instead via R17 to IO18
#define SUNTON_ESP32_TOUCH_PIN_INT              GPIO_NUM_NC

// interupt pin was falsely routed to GND, so its 0x5D
#define SUNTON_ESP32_TOUCH_ADDRESS              ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS

// not required, external pullups R3 / R4 in place
//#define SUNTON_ESP32_TOUCH_I2C_PULLUP           y

#define SUNTON_ESP32_BACKLIGHT_LEDC_TIMER       LEDC_TIMER_0
#define SUNTON_ESP32_BACKLIGHT_LEDC_CHANNEL     LEDC_CHANNEL_0

// I2S Audio pins (MAX98357 amplifier)
#define SUNTON_ESP32_I2S_BCLK                   GPIO_NUM_0   // Bit clock (v1.1)
#define SUNTON_ESP32_I2S_LRCLK                  GPIO_NUM_18  // Word select
#define SUNTON_ESP32_I2S_DIN                    GPIO_NUM_17  // Data in

#define LVGL_TICK_PERIOD_MS                     2

#ifdef __cplusplus
extern "C" {
#endif

void sunton_esp32s3_backlight_init(void);
lv_display_t *sunton_esp32s3_lcd_init(void);
void sunton_esp32s3_lcd_force_refresh(void);
i2c_master_bus_handle_t sunton_esp32s3_i2c_master(void);
void sunton_esp32s3_touch_init(i2c_master_bus_handle_t i2c_master);

#ifdef __cplusplus
}
#endif
