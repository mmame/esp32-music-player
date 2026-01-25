# ESP32 Music Player - based on ESP32-8048S050C

**Music Player with Web Interface for ESP32-S3 Touch Display**

A music player application for the Sunton ESP32-S3 800x480 capacitive touch display ESP32-8048S050C, built with FreeRTOS, LVGL 9.3, and featuring MP3/WAV playback capabilities.

## Features

- MP3 and WAV audio playback with minimp3 decoder
- Touch-based UI with LVGL 9.3
- Web-based configuration interface
- File management system
- WiFi configuration
- Physical Button mapping configuration

## Hardware

This project is designed for the Sunton ESP32-S3 8048S050C display:
- ESP32-S3 microcontroller
- 800x480 RGB LCD display
- GT911 capacitive touch controller
- Audio output support

## Based On

This project is based on [mr-sven/esp32-8048S050C](https://github.com/mr-sven/esp32-8048S050C) - thanks for the excellent hardware implementation foundation!

## Building

* Set esp-idf target to ESP32S3
* The supplied sdkconfig.defaults configures SPIRAM
* Requires esp-idf 5.5+

```bash
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```
