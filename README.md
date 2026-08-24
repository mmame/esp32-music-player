# esp32-music-player

Compact ESP32-based music player with crank-driven playback speed, dimmer/light-organ output, and two coordinated firmwares (player + display).

## Hardware Overview

* Main controller: Custom ESP32-S3 board running the player firmware.
* Display controller: separate ESP32 board for touchscreen UI.
* Storage: SD card (audio files and optional per-song JSON settings).
* Audio output: XLR and asymmetric Analog output.
* Inputs: rotary/crank encoder(s), potentiometer(s), buttons.
* Lighting: DimmerLink output with optional FFT-based light-organ mode.
* Connectivity: UART link between player and display, optional Wi-Fi web UI.

## Firmware Overview

### Player Firmware

Path: firmware/player

Main responsibilities:

* Audio pipeline (SD -> decode -> SoundTouch/downmix -> volume -> I2S)
* Song/playlist handling and per-song settings
* Crank-based tempo and playback state logic
* Web UI for file management and configuration
* UART protocol handling for display sync and commands

### Display Firmware

Path: firmware/display

Main responsibilities:

* Touchscreen user interface
* Live control commands (playback, settings, downmix, etc.)
* Song settings editing and transmission over UART
* Status rendering from player state updates

## Repository Layout

* firmware/player: ESP-IDF + ESP-ADF player firmware
* firmware/display: touchscreen/display firmware
* hardware: hardware-related assets/docs
* tools: helper scripts and utilities

## Project Link

* https://github.com/mmame/esp32-music-player

