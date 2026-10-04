# esp32-music-player

Compact ESP32-based music player with crank-driven playback speed, dimmer/light-organ output, and two coordinated firmwares (player + display).

## Hardware Overview

* Main controller: Custom ESP32-S3 board running the player firmware.
* Display controller: separate ESP32 / ESP32-S3 board for the touchscreen UI (see [Supported Displays](#supported-displays)).
* Storage: SD card (audio files and optional per-song JSON settings).
* Audio output: XLR and asymmetric Analog output.
* Inputs: rotary/crank encoder(s), potentiometer(s), buttons.
* Lighting: DimmerLink output with optional FFT-based light-organ mode.
* Connectivity: UART link between player and display, optional Wi-Fi web UI.

## Firmware Overview

### Player Firmware

Path: firmware/player

Main responsibilities:

* Audio pipeline (SD -> decode -> SoundTouch/downmix -> volume -> gain/limiter -> I2S)
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

#### Supported Displays

Two display firmware variants are built from the same source (board driver and UI layout are selected per variant):

| Display board | Chip | Screen | Touch | Firmware variant | UI layout |
| --- | --- | --- | --- | --- | --- |
| ESP32-8048S050C (Sunton 5") | ESP32-S3 | 800x480 RGB | GT911 | `8048S050C` | full |
| ESP32-8048S043 (Sunton 4.3") | ESP32-S3 | 800x480 RGB | GT911 | `8048S050C` (same firmware, no separate build) | full |
| ESP32-2432S032C (Sunton 3.2") | ESP32 | 320x240 SPI (ST7789) | GT911 | `2432S032` | compact |

* The 4.3" ESP32-8048S043 runs the 8048S050C firmware: the touch controller's coordinate range is read at startup and scaled to the screen.
* The compact layout for 320x240 uses larger text, drops the VOL/TMP bars and arranges the controls in two rows.
* Build both variants with `firmware/display/build_all.ps1`. It produces one merged image per variant (bootloader + partition table + app) in `firmware/display/release/`.
* Flash a display through the player's web interface (Firmware Update page): the player detects the display chip and picks the flash address itself (address `auto`). Details are in [firmware/display/README.md](firmware/display/README.md).
* The player's About page also shows the display firmware's version, build and resolution.

## Operating Modes

The player has two modes, selected on the web interface (Configuration -> Operating mode). The default is crank mode; a change applies live.

### Crank mode (default)

* The crank starts, stops and speeds up playback: turning it resumes the song, stopping it fades out and pauses, and its speed sets the playback tempo (TMP).
* The lamp (dimmer) follows the crank speed, or the audio when light organ is enabled.
* Per-song settings (end-of-song action, fixed speed, pitch, dimmer, downmix, gain) are editable on the touchscreen and on the web interface.

### Player mode

* A "normal" player: the player screen has Prev, Play/Pause (one button), Next and Stop, plus a button that cycles what happens when a song ends (Stop / Next / Repeat).
* The crank is ignored, speed is fixed at 1.0x, and the VOL/TMP bars and the per-song settings gear are hidden on the touchscreen (on the 800x480 display the player screen then uses the full width). The end-of-song button replaces the per-song setting.
* Physical buttons on the ADC ladder can be assigned to these functions on the web interface (Buttons tab): press "Assign", then the button. A button can have one function per screen (player screen / song list), but may be reused on the other screen, for example "Next" and "Down". Assigned buttons are active in player mode only.
* Play/Pause fades the volume and lamp in and out like the crank does.

## Output Gain

The global output gain (0 to +10 dB, default +6 dB) is set on the web interface, and each song can add -6 to +6 dB. The total is capped at +10 dB and passes through a peak limiter (-1 dBFS) so boosted audio does not clip.

## Building and Releases

Run `.\build_all.ps1` (PowerShell 7) in the repository root. It builds the player firmware (ESP-IDF 5.5.4 + ESP-ADF) and both display
variants (ESP-IDF 5.5.2) and collects the binaries in `release/`, with the build date (`yyyymmdd`) in every file name:

| File | What it is | How to flash |
| --- | --- | --- |
| `musicplayer-<date>.bin` | player application image | upload on the player's web update page (OTA) |
| `ESP32-8048S050C-full-<date>.bin` | display firmware, 800x480 (also for ESP32-8048S043) | player web update page, address `auto` |
| `ESP32-2432S032C-full-<date>.bin` | display firmware, 320x240 | player web update page, address `auto` |

Options: `-Clean` does a clean build, `-SkipPlayer` / `-SkipDisplay` build only one side, `-Date` overrides the date stamp.
The individual firmware scripts are `firmware/player/build_all.ps1` and `firmware/display/build_all.ps1`.

## Repository Layout

* firmware/player: ESP-IDF + ESP-ADF player firmware
* firmware/display: touchscreen/display firmware
* release: release binaries with date stamp (built by `build_all.ps1`)
* hardware: hardware-related assets/docs
* tools: helper scripts and utilities

## Project Link

* https://github.com/mmame/esp32-music-player

