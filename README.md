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

## Operating Modes

The player has two modes, selected on the web interface (Configuration -> Operating mode). The default is crank mode; a change applies live.

### Crank mode (default)

* The crank starts, stops and speeds up playback: turning it resumes the song, stopping it fades out and pauses, and its speed sets the playback tempo (TMP).
* The lamp (dimmer) follows the crank speed, or the audio when light organ is enabled.
* Per-song settings (end-of-song action, fixed speed, pitch, dimmer, downmix, gain) are editable on the touchscreen and on the web interface.

### Player mode

* A "normal" player: the player screen has Prev, Play/Pause (one button), Next and Stop, plus a button that cycles what happens when a song ends (Stop / Next / Repeat).
* The crank is ignored, speed is fixed at 1.0x, and the TMP bar and per-song settings gear are hidden on the touchscreen. The end-of-song button replaces the per-song setting.
* Physical buttons on the ADC ladder can be assigned to these functions on the web interface (Buttons tab): press "Assign", then the button. A button can have one function per screen (player screen / song list), but may be reused on the other screen, for example "Next" and "Down". Assigned buttons are active in player mode only.
* Play/Pause fades the volume and lamp in and out like the crank does.

## Output Gain

The global output gain (0 to +10 dB, default +6 dB) is set on the web interface, and each song can add -6 to +6 dB. The total is capped at +10 dB and passes through a peak limiter (-1 dBFS) so boosted audio does not clip.

## Repository Layout

* firmware/player: ESP-IDF + ESP-ADF player firmware
* firmware/display: touchscreen/display firmware
* hardware: hardware-related assets/docs
* tools: helper scripts and utilities

## Project Link

* https://github.com/mmame/esp32-music-player

