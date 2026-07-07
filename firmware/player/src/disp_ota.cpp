/**
 * @file disp_ota.cpp
 * @brief Remote firmware-update for the display ESP32.
 *
 * Implements the ESP32 ROM UART bootloader protocol (SLIP-framed) to flash
 * a firmware binary over UART1 (the same pins used for normal display comms).
 *
 * Protocol reference:
 *   https://docs.espressif.com/projects/esptool/en/latest/esp32/advanced-topics/serial-protocol.html
 *
 * Key commands used (ROM loader, no stub):
 *   SYNC         (0x08) – synchronise
 *   SPI_ATTACH   (0x0D) – attach SPI flash
 *   CHANGE_BAUD  (0x0F) – speed up after initial sync
 *   FLASH_BEGIN  (0x02) – erase + prepare write region
 *   FLASH_DATA   (0x03) – write one 16 KB block
 *   FLASH_END    (0x04) – finalise + reboot
 */

#include "disp_ota.h"
#include "pins.h"
#include "uart_master.h"

#include "dimmerlink.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

static const char *TAG = "disp_ota";

/* ── Constants ──────────────────────────────────────────────────────── */

#define SLIP_END      0xC0u
#define SLIP_ESC      0xDBu
#define SLIP_ESC_END  0xDCu
#define SLIP_ESC_ESC  0xDDu

/* Typed constants for passing to uart_write_bytes without braced-init casts. */
static const uint8_t SLIP_ESC_BYTE    = 0xDB;
static const uint8_t SLIP_END_BYTE    = 0xC0;

#define ROM_SYNC        0x08u
#define ROM_SPI_ATTACH  0x0Du
#define ROM_CHANGE_BAUD 0x0Fu
#define ROM_FLASH_BEGIN 0x02u
#define ROM_FLASH_DATA  0x03u
#define ROM_FLASH_END   0x04u

#define FLASH_BLOCK_SIZE  0x400u    /* 1 KB per FLASH_DATA block (ROM loader; stub uses 0x4000) */
#define OTA_BAUD_RATE     460800    /* baud after CHANGE_BAUDRATE                       */

#define CSUM_MAGIC 0xEFu            /* XOR-checksum seed for FLASH_DATA                 */

static constexpr uart_port_t OTA_PORT = static_cast<uart_port_t>(UM_UART_NUM);

/* ── Progress helper ─────────────────────────────────────────────────── */

static void progress(httpd_req_t *req, const char *fmt, ...)
{
    char buf[220];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s", buf);
    if (req) {
        size_t len = strlen(buf);
        buf[len]   = '\n';
        buf[len+1] = '\0';
        httpd_resp_send_chunk(req, buf, (ssize_t)(len + 1));
    }
}

/* ── GPIO helpers ────────────────────────────────────────────────────── */

static inline void rst_set(int level)
{
    gpio_set_level((gpio_num_t)DISP_ESP32_RESET_PIN, level);
}

static inline void boot0_set(int level)
{
    gpio_set_level((gpio_num_t)DISP_ESP32_BOOT0_PIN, level);
}

/* ── SLIP streaming send ─────────────────────────────────────────────── */

/** SLIP-encode and transmit `len` bytes (no surrounding 0xC0 end-markers). */
static void slip_encode_bytes(const uint8_t *data, size_t len)
{
    const uint8_t *p   = data;
    const uint8_t *end = data + len;
    while (p < end) {
        /* Find the next byte that needs escaping. */
        const uint8_t *q = p;
        while (q < end && *q != SLIP_END && *q != SLIP_ESC) ++q;
        /* Flush the clean run. */
        if (q > p) uart_write_bytes(OTA_PORT, p, (size_t)(q - p));
        /* Escape the special byte. */
        if (q < end) {
            /* Write the escape prefix, then the escaped byte value. */
            uart_write_bytes(OTA_PORT, &SLIP_ESC_BYTE, 1);
            uint8_t mapped = (uint8_t)((*q == SLIP_END) ? SLIP_ESC_END : SLIP_ESC_ESC);
            uart_write_bytes(OTA_PORT, &mapped, 1);
            ++q;
        }
        p = q;
    }
}

/**
 * Build and transmit a ROM bootloader request packet.
 *
 * Frame: 0xC0 [hdr:8 bytes] [data:dlen bytes] 0xC0  (all SLIP-encoded)
 * Header layout: [dir=0x00][cmd][size_lo][size_hi][csum:4 LE]
 */
static void rom_send_cmd(uint8_t cmd,
                         const uint8_t *data, uint16_t dlen,
                         uint32_t csum)
{
    uint8_t hdr[8];
    hdr[0] = 0x00;  /* direction: request */
    hdr[1] = cmd;
    hdr[2] = (uint8_t)(dlen & 0xFF);
    hdr[3] = (uint8_t)(dlen >> 8);
    hdr[4] = (uint8_t)(csum        & 0xFF);
    hdr[5] = (uint8_t)((csum >>  8) & 0xFF);
    hdr[6] = (uint8_t)((csum >> 16) & 0xFF);
    hdr[7] = (uint8_t)((csum >> 24) & 0xFF);

    const uint8_t frame_end = SLIP_END;
    uart_write_bytes(OTA_PORT, &frame_end, 1);
    slip_encode_bytes(hdr, 8);
    if (data && dlen > 0) slip_encode_bytes(data, dlen);
    uart_write_bytes(OTA_PORT, &frame_end, 1);
}

/* ── SLIP streaming receive ──────────────────────────────────────────── */

/**
 * Read and SLIP-decode one frame into `out[maxlen]`.
 *
 * Waits up to `timeout_ms` for a complete frame.
 * Returns the decoded byte count, or -1 on timeout.
 */
static int rom_recv_frame(uint8_t *out, int maxlen, int timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000LL;
    int     len      = 0;
    bool    in_frame = false;
    bool    escaped  = false;
    uint8_t b;

    while (esp_timer_get_time() < deadline) {
        int remaining_ms = (int)((deadline - esp_timer_get_time()) / 1000LL);
        if (remaining_ms <= 0) break;
        int wait = (remaining_ms < 10) ? remaining_ms : 10;

        if (uart_read_bytes(OTA_PORT, &b, 1, pdMS_TO_TICKS(wait)) != 1) continue;

        if (b == SLIP_END) {
            if (in_frame && len > 0) return len;   /* complete frame */
            in_frame = true;                        /* start of frame */
            len = 0; escaped = false;
            continue;
        }
        if (!in_frame) continue;

        if (b == SLIP_ESC) { escaped = true; continue; }
        if (escaped) {
            escaped = false;
            b = (b == SLIP_ESC_END) ? SLIP_END : SLIP_ESC;
        }
        if (len < maxlen) out[len++] = b;
    }
    return -1; /* timeout */
}

/**
 * Discard frames until we find a valid response for `cmd`.
 * A valid response has: frame[0]==0x01, frame[1]==cmd, frame[8]==0x00 (OK).
 * Returns true on success, false on timeout or error status.
 */
static bool rom_wait_resp(uint8_t cmd, int timeout_ms)
{
    uint8_t frame[64];
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000LL;

    while (esp_timer_get_time() < deadline) {
        int remaining_ms = (int)((deadline - esp_timer_get_time()) / 1000LL);
        if (remaining_ms <= 0) break;

        int len = rom_recv_frame(frame, (int)sizeof(frame), remaining_ms);
        if (len < 10) continue;
        if (frame[0] != 0x01) continue;   /* not a response direction */
        if (frame[1] != cmd)  continue;   /* wrong command echo       */
        if (frame[8] != 0x00) {
            ESP_LOGW(TAG, "ROM cmd 0x%02X error status=0x%02X error_code=0x%02X",
                     cmd, frame[8], frame[9]);
            return false;
        }
        return true;
    }
    ESP_LOGW(TAG, "ROM cmd 0x%02X: response timeout (%d ms)", cmd, timeout_ms);
    return false;
}

/* ── ROM bootloader commands ─────────────────────────────────────────── */

static bool rom_sync(void)
{
    /* Sync payload: 0x07 0x07 0x12 0x20  followed by 32 × 0x55 */
    static const uint8_t payload[36] = {
        0x07, 0x07, 0x12, 0x20,
        0x55,0x55,0x55,0x55,0x55,0x55,0x55,0x55,
        0x55,0x55,0x55,0x55,0x55,0x55,0x55,0x55,
        0x55,0x55,0x55,0x55,0x55,0x55,0x55,0x55,
        0x55,0x55,0x55,0x55,0x55,0x55,0x55,0x55,
    };

    vTaskDelay(pdMS_TO_TICKS(20));
    uart_flush_input(OTA_PORT);

    /*
     * Use the same burst strategy as esptool:
     * send several SYNC packets in rapid succession so the ROM bootloader
     * can latch onto one even if the first bytes arrive mid-frame.
     * 25 outer attempts × (5 sends + 100 ms wait) ≈ 12 s max.
     */
    for (int attempt = 0; attempt < 25; attempt++) {
        /* Burst of 5 SYNC packets with minimal gap */
        for (int burst = 0; burst < 5; burst++) {
            rom_send_cmd(ROM_SYNC, payload, (uint16_t)sizeof(payload), 0);
            vTaskDelay(pdMS_TO_TICKS(5));
        }

        uint8_t frame[64];
        int len = rom_recv_frame(frame, (int)sizeof(frame), 200);

        if (len > 0) {
            /* Log what we got – helps diagnose polarity / baud issues */
            ESP_LOGI(TAG, "SYNC rx %d bytes: %02X %02X %02X %02X %02X %02X %02X %02X ...",
                     len,
                     len>0?frame[0]:0, len>1?frame[1]:0, len>2?frame[2]:0, len>3?frame[3]:0,
                     len>4?frame[4]:0, len>5?frame[5]:0, len>6?frame[6]:0, len>7?frame[7]:0);
        }

        if (len >= 10 && frame[0] == 0x01 && frame[1] == ROM_SYNC && frame[8] == 0x00) {
            /* Flush any additional SYNC response frames the ROM may keep sending. */
            vTaskDelay(pdMS_TO_TICKS(50));
            uart_flush_input(OTA_PORT);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return false;
}

static bool rom_spi_attach(void)
{
    static const uint8_t payload[8] = {0};   /* all zeros for standard SPI flash */
    rom_send_cmd(ROM_SPI_ATTACH, payload, 8, 0);
    return rom_wait_resp(ROM_SPI_ATTACH, 1000);
}

static void ota_set_baud(uint32_t baud);   /* forward declaration */

static bool rom_change_baud(uint32_t new_baud)
{
    uint8_t payload[8] = {};
    payload[0] = (uint8_t)(new_baud        & 0xFF);
    payload[1] = (uint8_t)((new_baud >>  8) & 0xFF);
    payload[2] = (uint8_t)((new_baud >> 16) & 0xFF);
    payload[3] = (uint8_t)((new_baud >> 24) & 0xFF);
    /* payload[4..7] = 0: "old baud = 0" means the ROM derives it itself */

    rom_send_cmd(ROM_CHANGE_BAUD, payload, 8, 0);
    if (!rom_wait_resp(ROM_CHANGE_BAUD, 1000)) return false;

    /* Both sides switch simultaneously. */
    ota_set_baud(new_baud);
    return true;
}

/**
 * FLASH_BEGIN: erase the flash region and prepare for writing.
 *
 * For ESP32-S3 (and all non-ESP8266 chips), esp-serial-flasher sets
 * encryption_in_begin_flash_cmd = true, which means the payload is 20 bytes:
 *   [erase_size][packet_count][packet_size][offset][encrypted]
 * Sending only 16 bytes (no encrypted field) causes the ROM to return
 * INVALID_COMMAND (0x05) because the size field in the header is wrong.
 * erase_size = fw_size directly (not sector-aligned) — the ROM handles
 * alignment internally. calc_erase_size() in esp-serial-flasher confirms this.
 */
static bool rom_flash_begin(uint32_t fw_size, uint32_t addr)
{
    uint32_t num_blocks = (fw_size + FLASH_BLOCK_SIZE - 1u) / FLASH_BLOCK_SIZE;
    uint32_t erase_size = fw_size;   /* ROM aligns erase internally */

    ESP_LOGI(TAG, "FLASH_BEGIN: erase_size=%lu num_blocks=%lu block_size=%u addr=0x%08lX encrypted=0",
             (unsigned long)erase_size, (unsigned long)num_blocks,
             FLASH_BLOCK_SIZE, (unsigned long)addr);

    /* 20-byte payload: 5 × uint32 LE                                      */
    uint8_t payload[20] = {};
    /* erase_size */
    payload[0]  = (uint8_t)(erase_size         & 0xFF);
    payload[1]  = (uint8_t)((erase_size >>  8)  & 0xFF);
    payload[2]  = (uint8_t)((erase_size >> 16)  & 0xFF);
    payload[3]  = (uint8_t)((erase_size >> 24)  & 0xFF);
    /* num_blocks */
    payload[4]  = (uint8_t)(num_blocks         & 0xFF);
    payload[5]  = (uint8_t)((num_blocks >>  8)  & 0xFF);
    payload[6]  = (uint8_t)((num_blocks >> 16)  & 0xFF);
    payload[7]  = (uint8_t)((num_blocks >> 24)  & 0xFF);
    /* block_size */
    payload[8]  = (uint8_t)(FLASH_BLOCK_SIZE        & 0xFF);
    payload[9]  = (uint8_t)((FLASH_BLOCK_SIZE >>  8) & 0xFF);
    payload[10] = (uint8_t)((FLASH_BLOCK_SIZE >> 16) & 0xFF);
    payload[11] = (uint8_t)((FLASH_BLOCK_SIZE >> 24) & 0xFF);
    /* flash address */
    payload[12] = (uint8_t)(addr         & 0xFF);
    payload[13] = (uint8_t)((addr >>  8)  & 0xFF);
    payload[14] = (uint8_t)((addr >> 16)  & 0xFF);
    payload[15] = (uint8_t)((addr >> 24)  & 0xFF);
    /* encrypted = 0 (already zero from = {}) — required for ESP32-S3 ROM  */
    /* payload[16..19] = 0x00000000                                         */

    rom_send_cmd(ROM_FLASH_BEGIN, payload, 20, 0);   /* 20 bytes for ESP32-S3 */
    /* Erase can be slow: allow up to 60 s for a large region. */
    return rom_wait_resp(ROM_FLASH_BEGIN, 60000);
}

/**
 * FLASH_DATA: write one full FLASH_BLOCK_SIZE block.
 *
 * `block_data` must point to exactly FLASH_BLOCK_SIZE bytes, padded with 0xFF.
 * `seq` is the 0-based block sequence number.
 */
static bool rom_flash_data_block(const uint8_t *block_data, uint32_t seq)
{
    /* XOR checksum of the data bytes, seeded with CSUM_MAGIC (0xEF). */
    uint32_t csum = CSUM_MAGIC;
    for (uint32_t i = 0; i < FLASH_BLOCK_SIZE; i++) csum ^= block_data[i];

    /*
     * FLASH_DATA sub-header (16 bytes):
     *   [data_size:4][seq:4][0x00000000:4][0x00000000:4]
     */
    uint8_t sub_hdr[16] = {};
    sub_hdr[0] = (uint8_t)(FLASH_BLOCK_SIZE        & 0xFF);
    sub_hdr[1] = (uint8_t)((FLASH_BLOCK_SIZE >>  8) & 0xFF);
    sub_hdr[2] = (uint8_t)((FLASH_BLOCK_SIZE >> 16) & 0xFF);
    sub_hdr[3] = (uint8_t)((FLASH_BLOCK_SIZE >> 24) & 0xFF);
    sub_hdr[4] = (uint8_t)(seq         & 0xFF);
    sub_hdr[5] = (uint8_t)((seq >>  8)  & 0xFF);
    sub_hdr[6] = (uint8_t)((seq >> 16)  & 0xFF);
    sub_hdr[7] = (uint8_t)((seq >> 24)  & 0xFF);
    /* sub_hdr[8..15] already 0 (padding) */

    uint16_t dlen = 16u + FLASH_BLOCK_SIZE;

    /* Build and send the outer 8-byte command header, then the payload. */
    uint8_t outer_hdr[8];
    outer_hdr[0] = 0x00;
    outer_hdr[1] = ROM_FLASH_DATA;
    outer_hdr[2] = (uint8_t)(dlen & 0xFF);
    outer_hdr[3] = (uint8_t)(dlen >> 8);
    outer_hdr[4] = (uint8_t)(csum        & 0xFF);
    outer_hdr[5] = (uint8_t)((csum >>  8) & 0xFF);
    outer_hdr[6] = (uint8_t)((csum >> 16) & 0xFF);
    outer_hdr[7] = (uint8_t)((csum >> 24) & 0xFF);

    const uint8_t frame_end = SLIP_END;
    uart_write_bytes(OTA_PORT, &frame_end, 1);
    slip_encode_bytes(outer_hdr, 8);
    slip_encode_bytes(sub_hdr, 16);
    slip_encode_bytes(block_data, FLASH_BLOCK_SIZE);
    uart_write_bytes(OTA_PORT, &frame_end, 1);

    return rom_wait_resp(ROM_FLASH_DATA, 5000);
}

static bool rom_flash_end(bool reboot)
{
    /* payload[0]: 0 = reboot after flash, 1 = stay in flash mode */
    uint8_t payload[4] = { reboot ? 0u : 0u, 0u, 0u, 0u };
    rom_send_cmd(ROM_FLASH_END, payload, 4, 0);
    /* The display may reset before we receive the ACK – treat timeout as OK. */
    rom_wait_resp(ROM_FLASH_END, 1000);
    return true;
}

/* ── UART reconfiguration for OTA ────────────────────────────────────── */

/**
 * Switch the already-installed UART1 driver to a new baud rate.
 * uart_master_pause() has already set 115200; this is only used by
 * rom_change_baud() to switch to the faster OTA rate mid-session.
 * (No driver install/delete needed – the GPIO pins stay assigned.)
 */
static void ota_set_baud(uint32_t baud)
{
    uart_wait_tx_done(OTA_PORT, pdMS_TO_TICKS(100));
    uart_set_baudrate(OTA_PORT, baud);
    /* Give the physical UART lines time to settle after the baud switch.  *
     * Both sides must be stable before the next command is sent.         */
    vTaskDelay(pdMS_TO_TICKS(80));
    uart_flush_input(OTA_PORT);
    vTaskDelay(pdMS_TO_TICKS(20));  /* extra settle after flush             */
    uart_flush_input(OTA_PORT);     /* clear any bytes that arrived during delay */
}

/* ══════════════════════════════════════════════════════════════════════
 * Public API
 * ══════════════════════════════════════════════════════════════════════ */

void disp_ota_init(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = (1ULL << DISP_ESP32_RESET_PIN),   /* BOOT0 owned by I2C */
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    gpio_set_level((gpio_num_t)DISP_ESP32_RESET_PIN, 1); /* deassert reset */
    /* BOOT0 (GPIO4) is shared with I2C SCL.  The external 4.7 kΩ pull-up  *
     * holds it HIGH (= normal boot) during normal operation; disp_ota     *
     * suspends I2C and drives it LOW only during OTA flashing.            */
    ESP_LOGI(TAG, "Display pins initialised: RST=HIGH (BOOT0 managed by I2C)");
}

esp_err_t disp_ota_flash(const char *path, uint32_t flash_addr, httpd_req_t *req)
{
    esp_err_t result  = ESP_FAIL;
    uint8_t  *block   = nullptr;
    FILE     *fw_file = nullptr;

    /* ── 1. Stat firmware file ──────────────────────────────────────── */
    struct stat st;
    if (stat(path, &st) != 0) {
        progress(req, "ERROR: Cannot stat firmware file: %s", path);
        goto done;
    }
    {
        uint32_t fw_size    = (uint32_t)st.st_size;
        uint32_t num_blocks = (fw_size + FLASH_BLOCK_SIZE - 1u) / FLASH_BLOCK_SIZE;
        progress(req, "Firmware: %lu bytes  |  %lu blocks x %u bytes",
                 (unsigned long)fw_size, (unsigned long)num_blocks, FLASH_BLOCK_SIZE);

        /* ── 2. Allocate per-block working buffer ─────────────────────── */
        block = static_cast<uint8_t *>(malloc(FLASH_BLOCK_SIZE));
        if (!block) {
            progress(req, "ERROR: Out of heap memory");
            goto done;
        }

        /* ── 3. Pause uart_master (baud already switched to 115200) ──── */
        progress(req, "Pausing normal UART link...");
        uart_master_pause();

        progress(req, "Entering display bootloader (BOOT0=0/LOW, RST pulse)...");

        /* BOOT0 pin (GPIO4) is shared with I2C SCL. Suspend I2C first,   *
         * then configure the pin as a plain GPIO output so we can drive  *
         * it LOW to select download mode.                                */
        dimmerlink_suspend();
        {
            const gpio_config_t boot0_cfg = {
                .pin_bit_mask = (1ULL << DISP_ESP32_BOOT0_PIN),
                .mode         = GPIO_MODE_OUTPUT,
                .pull_up_en   = GPIO_PULLUP_DISABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .intr_type    = GPIO_INTR_DISABLE,
            };
            gpio_config(&boot0_cfg);
        }
        boot0_set(0);                    /* GPIO0 LOW = download mode         */
        vTaskDelay(pdMS_TO_TICKS(500));  /* wait for cap to fully discharge   */
        rst_set(0);      
        uart_flush_input(OTA_PORT);
        vTaskDelay(pdMS_TO_TICKS(200));  /* hold RST low                      */
        rst_set(1);                      /* release reset – ROM reads GPIO0   */

        /* Wait for ROM bootloader to start, drain and log its banner.       *
         * In download mode the ROM prints "Waiting for download\n".         *
         * In normal (app) mode you only see the app boot log or nothing.    *
         * Read up to 256 bytes over 600 ms to capture the full banner.      */
        vTaskDelay(pdMS_TO_TICKS(500));
        {
            uint8_t boot_msg[256];
            int n = 0;
            /* Drain UART in 50 ms chunks for up to 600 ms total.           */
            int64_t drain_end = esp_timer_get_time() + 600LL * 1000LL;
            while (n < (int)sizeof(boot_msg) - 1 &&
                   esp_timer_get_time() < drain_end) {
                int got = uart_read_bytes(OTA_PORT,
                                         boot_msg + n,
                                         (int)sizeof(boot_msg) - 1 - n,
                                         pdMS_TO_TICKS(50));
                if (got > 0) n += got; else break;
            }
            boot_msg[n] = '\0';   /* NUL-terminate for ASCII printing     */

            if (n > 0) {
                /* Print first 8 hex bytes for framing reference. */
                ESP_LOGI(TAG, "Boot banner: %d bytes  hex: "
                         "%02X %02X %02X %02X %02X %02X %02X %02X ...",
                         n,
                         n>0?boot_msg[0]:0, n>1?boot_msg[1]:0,
                         n>2?boot_msg[2]:0, n>3?boot_msg[3]:0,
                         n>4?boot_msg[4]:0, n>5?boot_msg[5]:0,
                         n>6?boot_msg[6]:0, n>7?boot_msg[7]:0);
                /* Sanitise non-printable bytes for the ASCII log line.     */
                for (int i = 0; i < n; i++) {
                    if (boot_msg[i] < 0x20 && boot_msg[i] != '\n' &&
                        boot_msg[i] != '\r') boot_msg[i] = '.';
                }
                ESP_LOGI(TAG, "Boot banner text: %s", (char *)boot_msg);

                bool download_mode = (strstr((char *)boot_msg,
                                            "waiting for download") != nullptr) ||
                                     (strstr((char *)boot_msg,
                                            "Waiting for download") != nullptr);
                if (download_mode) {
                    progress(req, "Display is in download mode (Waiting for download).");
                } else {
                    progress(req, "WARNING: 'Waiting for download' not found – "
                             "display may have booted normally (BOOT0 polarity?)");
                }
            } else {
                ESP_LOGW(TAG, "Boot banner: no bytes received – "
                         "check BOOT0 wiring / TX-RX connection");
                progress(req, "Warning: display sent nothing after reset "
                         "(BOOT0 wiring? TX/RX swap?)");
            }
            uart_flush_input(OTA_PORT);
        }

        /* UART driver stays installed at 115200 – no reinstall needed.   */

        /* ── 4. SYNC ─────────────────────────────────────────────────── */
        progress(req, "Syncing with ROM bootloader at 115200 baud...");
        if (!rom_sync()) {
            progress(req, "ERROR: SYNC failed – check wiring and display power");
            goto cleanup_uart;
        }
        progress(req, "SYNC OK");

        /* ── 6. SPI_ATTACH ───────────────────────────────────────────── */
        progress(req, "Attaching SPI flash...");
        if (!rom_spi_attach()) {
            progress(req, "ERROR: SPI_ATTACH failed");
            goto cleanup_uart;
        }
        progress(req, "SPI_ATTACH OK");

        /* ── 7. CHANGE_BAUDRATE (optional speed-up) ──────────────────── */
        progress(req, "Switching to %d baud...", OTA_BAUD_RATE);
        if (!rom_change_baud(OTA_BAUD_RATE)) {
            progress(req, "Warning: CHANGE_BAUDRATE failed – continuing at 115200");
            /* non-fatal */
        } else {
            progress(req, "Baud rate changed to %d", OTA_BAUD_RATE);
        }

        /* ── 8. FLASH_BEGIN (erase) ───────────────────────────────────── */
        vTaskDelay(pdMS_TO_TICKS(50));   /* settle after baud change         */
        progress(req, "Erasing flash: %lu bytes at 0x%08lX...",
                 (unsigned long)fw_size, (unsigned long)flash_addr);
        if (!rom_flash_begin(fw_size, flash_addr)) {
            progress(req, "ERROR: FLASH_BEGIN / erase failed");
            goto cleanup_uart;
        }
        progress(req, "Erase OK");

        /* ── 9. FLASH_DATA blocks ──────────────────────────────────────── */
        fw_file = fopen(path, "rb");
        if (!fw_file) {
            progress(req, "ERROR: Cannot reopen firmware file");
            goto cleanup_uart;
        }

        for (uint32_t seq = 0; seq < num_blocks; seq++) {
            memset(block, 0xFF, FLASH_BLOCK_SIZE);         /* pad with 0xFF  */
            size_t rd = fread(block, 1, FLASH_BLOCK_SIZE, fw_file);
            if (rd == 0 && seq < num_blocks - 1u) {
                progress(req, "ERROR: Unexpected EOF at block %lu", (unsigned long)seq);
                goto cleanup_file;
            }

            if (!rom_flash_data_block(block, seq)) {
                progress(req, "ERROR: FLASH_DATA failed at block %lu", (unsigned long)seq);
                goto cleanup_file;
            }

            int pct = (int)((seq + 1u) * 100u / num_blocks);
            progress(req, "Writing: block %lu/%lu  (%d%%)",
                     (unsigned long)(seq + 1u), (unsigned long)num_blocks, pct);
        }
        fclose(fw_file);
        fw_file = nullptr;

        /* ── 10. FLASH_END ───────────────────────────────────────────── */
        progress(req, "Finalising flash...");
        rom_flash_end(false);   /* reboot display */
        progress(req, "Flash complete!");
        result = ESP_OK;
        goto cleanup_uart;  /* fall through to cleanup */

cleanup_file:
        if (fw_file) { fclose(fw_file); fw_file = nullptr; }

cleanup_uart:
        /* ── 11. Restore normal operation ───────────────────────────── */
        /* No uart_driver_delete needed – driver was kept alive.        */
        boot0_set(1);                    /* BOOT0 HIGH = normal boot     */
        vTaskDelay(pdMS_TO_TICKS(500));
        rst_set(0);
        vTaskDelay(pdMS_TO_TICKS(200));
        rst_set(1);          /* display boots normally                  */

        /* Release BOOT0 pin so I2C can reclaim GPIO4.                  */
        gpio_reset_pin((gpio_num_t)DISP_ESP32_BOOT0_PIN);
        dimmerlink_resume();

        progress(req, "Resuming normal UART link...");
        uart_master_resume();
    }

done:
    free(block);
    return result;
}
