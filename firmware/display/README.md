# ESP32-8048S050C

**Implementation with FreeRTOS OSAL and LVGL 9.4**

Sunton ESP32-S3 800x480 Capacitive touch display

Example using esp-idf 5.5.2 and the esp_lcd_touch_gt911 and lvgl components.

In gt911_touch_init, a callback is registered to map the measured touch coordinates to display coordinates, see header file for information.

* Set esp-idf target to ESP32S3, other versions might lack rgb panel support.
* The supplied sdkconfig.defaults configures SPIRAM, regenerate your sdkconfig if needed.

idf.py set-target esp32s3 idf.py build flash monitor

## Additional infos and limitations

Due the reduced size of internal RAM of the ESP32S3, the framebuffer cannot be located in internal RAM.

This limitation leads to transfer problems via the DMA. Progmem and PSRAM share the same SPI bus for read and write data.

* Bounce Buffer Mode
  * Pixelclock at 18 MHz
  * Fluent medium animations
  * Lack of performance in slide and scroll
* Double Buffer Mode
  * Pixelclock at 14 MHz
  * Fluent medium animations
  * Slide and scroll more fluent

If you are facing issues in panel distortion or glitchy animations in your project, this may caused by the DMA/SPI bottleneck, so you must reduce the pixelclock.

In bounce buffer mode, be aware of the lvgl draw buffer which is located in the internal RAM by default and may cause OOM issues.

## Branches

* [Main](../../tree/main)
  * LVGL 9.4.0
  * LVGL requires 128kb RAM for demo widgets
  * can use OSAL via `CONFIG_LV_OS_FREERTOS`
  * can use double-FB and direct rendering

* [Test](../../tree/lvgl-test) - test branch


## Firmware variants

Two distinct firmwares, selected by `CONFIG_DISPLAY_BOARD_*` (board driver + UI layout):

| Variant | Board | Target | Display | Touch | UI |
| --- | --- | --- | --- | --- | --- |
| 8048S050C | ESP32-8048S050C, also **ESP32-8048S043** (use this variant) | esp32s3 | 800x480 RGB | GT911 | full |
| 2432S032 | ESP32-2432S032C (ESP32-D0WDQ6) | esp32 | 320x240 ST7789 (SPI) | GT911 | compact |

The ESP32-8048S043 (4.3", ESP32-S3) is supported by the 8048S050C variant: the GT911 touch range is read from the
controller at startup and scaled to the 800x480 display, so its different touch resolution needs no separate build.

Compact UI (320x240): large song names (Montserrat 28, Latin-1 diacritics supported) in list and player,
larger elapsed/total time, no VOL/TMP bars (the full-width player gets the space instead), controls in two rows.

Run `.\build_all.ps1` to build both variants. The firmware to flash is the merged **full image** (bootloader +
partition table + app): `release/ESP32-8048S050C-full.bin` and `release/ESP32-2432S032C-full.bin`.
Flash it with the player update page, flash address `auto` (0x1000 on ESP32, 0x0 on ESP32-S3, chosen from the
detected chip). The `release` folder contains only these `-full.bin` files; the app-only `.bin` files inside the
`build_*` folders are ESP-IDF intermediates and are not meant to be flashed.

Build 800x480 (S3, default `sdkconfig`):

    idf.py set-target esp32s3 && idf.py build flash monitor

Build 320x240 (separate sdkconfig and build dir, never mixes with the S3 build):

    idf.py -B build_2432s032 -D SDKCONFIG=sdkconfig.2432s032 -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults.2432s032" set-target esp32
    idf.py -B build_2432s032 -D SDKCONFIG=sdkconfig.2432s032 -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults.2432s032" build flash monitor

The player link uses GPIO1 (TX) / GPIO3 (RX) on the 2432S032 (`uart_comm.h`): these are the ESP32 ROM download-UART pins, which the player's OTA flasher needs.
First flash over USB: bootloader at 0x1000, partition table at 0x8000, app at 0x10000 (the S3 uses 0x0 for the bootloader); later updates can use the player OTA (app only, 0x10000).
Orientation / colour / touch-mirror flags are in `main/sunton_esp32_2432s032.h`.
For the S3 variant, delete `sdkconfig` once so the new Montserrat 36/40 font options and the board choice are picked up.
