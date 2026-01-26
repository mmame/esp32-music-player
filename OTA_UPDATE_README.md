# OTA (Over-The-Air) Update Feature

## Overview
The ESP32 Music Player now supports OTA firmware updates directly from GitHub releases. Users can check for updates and install them with just a few taps on the About page.

## How It Works

### 1. Partition Table
The firmware now uses a **Two OTA** partition layout:
- **ota_0**: 2 MB - First app partition
- **ota_1**: 2 MB - Second app partition
- **otadata**: Stores which partition to boot from

When updating:
1. Device boots from one partition (e.g., ota_0)
2. New firmware downloads to the other partition (ota_1)
3. On success, device switches boot partition and reboots
4. If update fails, device can roll back to previous version

### 2. Version Management
- Current version defined in `main/ota_update.h`: `#define FIRMWARE_VERSION "1.0.0"`
- Update this version before each release

### 3. GitHub Release Setup

#### Required Files on GitHub Releases
Your GitHub release must contain these two files:

1. **firmware.bin** - The compiled firmware binary
2. **version.json** - Version information file

Example `version.json`:
```json
{
  "version": "1.0.1",
  "release_date": "2026-01-25",
  "release_notes": "Bug fixes and improvements"
}
```

#### Setting Up GitHub Releases

1. **Build your firmware:**
   ```bash
   idf.py build
   ```
   Binary is located at: `build/ESP32-8048S050C.bin`

2. **Copy version.json:**
   ```bash
   cp version.json build/
   ```

3. **Create GitHub Release:**
   - Go to: https://github.com/mmame/esp32-music-player/releases
   - Click "Draft a new release"
   - Create a new tag (e.g., `v1.0.1`)
   - Upload both files:
     - `build/ESP32-8048S050C.bin` → rename to `firmware.bin`
     - `version.json`
   - Publish release

4. **URLs:**
   The OTA system expects files at:
   - Firmware: `https://github.com/mmame/esp32-music-player/releases/latest/download/firmware.bin`
   - Version: `https://github.com/mmame/esp32-music-player/releases/latest/download/version.json`

### 4. Using the Update Feature

**On the Device:**
1. Navigate to the About page (swipe left from main screen)
2. Tap "Check for Updates"
3. Device connects to GitHub and checks version
4. If update available:
   - Shows current vs new version
   - Tap "Update" to proceed
   - Download progress shown
5. After download completes:
   - Firmware installs automatically
   - Device reboots with new version

## Configuration

### Changing GitHub URLs
Edit `main/ota_update.h`:
```c
#define GITHUB_RELEASE_URL "https://github.com/YOUR_USERNAME/YOUR_REPO/releases/latest/download/firmware.bin"
#define GITHUB_VERSION_URL "https://github.com/YOUR_USERNAME/YOUR_REPO/releases/latest/download/version.json"
```

### Version Format
Use semantic versioning: `MAJOR.MINOR.PATCH`
- Example: `1.0.0`, `1.2.3`, `2.0.0`

## Security Features

1. **HTTPS Only**: All downloads use encrypted HTTPS
2. **Certificate Validation**: Uses ESP-IDF certificate bundle
3. **Image Verification**: ESP32 validates firmware signature
4. **Rollback Protection**: Can revert to previous version if new version fails to boot

## Troubleshooting

### Update Check Fails
- Verify WiFi connection
- Check GitHub URLs are correct
- Ensure release files exist on GitHub

### Download Fails
- Check internet connectivity
- Verify firmware.bin is uploaded to release
- Check device has enough free memory

### Update Installs But Device Won't Boot
- Device automatically rolls back to previous version
- Check serial console for error messages
- Verify firmware was built for correct target (ESP32-S3)

### "No Update Available" When Update Exists
- Ensure version number in firmware code matches build
- Check version.json has different version than current
- Version comparison is string-based, so "1.0.10" > "1.0.9"

## Development Workflow

1. **Make changes to code**
2. **Update version in `main/ota_update.h`:**
   ```c
   #define FIRMWARE_VERSION "1.0.1"
   ```
3. **Build firmware:**
   ```bash
   idf.py build
   ```
4. **Create GitHub release** with firmware.bin and version.json
5. **Users can now update** from their devices

## Technical Details

### Components Used
- `esp_https_ota`: OTA download and installation
- `esp_http_client`: HTTP/HTTPS communication
- `esp_ota_ops`: Partition management and boot control
- `cJSON`: JSON parsing for version info

### Memory Requirements
- Download buffer: ~4 KB
- OTA partition: 2 MB
- Heap during update: ~40 KB

### Network Requirements
- Active WiFi connection
- Access to GitHub (port 443)
- Sufficient bandwidth for ~1.4 MB download

## Files Modified/Created

### New Files
- `main/ota_update.h` - OTA manager header
- `main/ota_update.cpp` - OTA manager implementation
- `version.json` - Version info template

### Modified Files
- `main/about_ui.cpp` - Added update UI
- `main/CMakeLists.txt` - Added OTA components
- `sdkconfig.defaults` - Changed to TWO_OTA partition table

## Future Enhancements

Potential improvements:
- [ ] Download progress percentage
- [ ] Release notes display
- [ ] Update scheduling
- [ ] Beta/stable channel selection
- [ ] Automatic update checks on startup
- [ ] Backup firmware to SD card before update
