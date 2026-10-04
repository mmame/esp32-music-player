# Release binaries

Built by `build_all.ps1` in the repository root. The date in each file name (`yyyymmdd`) is the build date.

| File | What it is | How to flash |
| --- | --- | --- |
| `musicplayer-<date>.bin` | Player application image | Upload on the player's web update page (OTA) |
| `ESP32-8048S050C-full-<date>.bin` | Display firmware, 800x480 (ESP32-S3; also for the ESP32-8048S043) | Player web update page, flash address `auto` |
| `ESP32-2432S032C-full-<date>.bin` | Display firmware, 320x240 (ESP32) | Player web update page, flash address `auto` |

Display images are merged images (bootloader + partition table + app); with flash address `auto` the player picks 0x1000 (ESP32)
or 0x0 (ESP32-S3) from the detected chip. See the main README for details.
