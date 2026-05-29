/**
 * @file dimmerlink.cpp
 * @brief Startup probe for the RBDimmer DimmerLink I2C module.
 *
 * Protocol references:
 *   I2C : https://www.rbdimmer.com/docs/dimmerlink-I2CCommunication
 *   UART: https://www.rbdimmer.com/docs/dimmerlink-UartCommunication
 *
 * The DimmerLink ships with UART mode active.  On startup this module:
 *   1. Tries I2C probe at 0x50.
 *   2. If no ACK, falls back to UART SWITCH_I2C (02 5B) on the same pins
 *      (SDA=GPIO4 as UART TX, SCL=GPIO0 as UART RX), 115200/8N1.
 *   3. After a successful switch, re-probes over I2C and logs device status.
 *
 * I2C connection parameters (from datasheet):
 *   Address : 0x50 (7-bit)
 *   Speed   : 100 kHz (Standard Mode)
 *   Pull-up : 4.7 kΩ on SDA and SCL (external, not internal)
 */

#include "dimmerlink.h"
#include "pins.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/uart.h"
#include "esp_log.h"

static const char *TAG = "dimmerlink";

/* ── Device constants ──────────────────────────────────────────────────── */

#define DIMMERLINK_I2C_ADDR    0x50u
#define DIMMERLINK_SPEED_HZ    100000

/* Register map */
#define REG_STATUS      0x00u  /* R   – bit0=READY, bit1=ERROR          */
#define REG_ERROR       0x02u  /* R   – last error code                 */
#define REG_VERSION     0x03u  /* R   – firmware version                */
#define REG_DIM0_LEVEL  0x10u  /* R/W – brightness 0–100%              */
#define REG_DIM0_CURVE  0x11u  /* R/W – dim curve (0=LINEAR,1=RMS,2=LOG)*/
#define REG_AC_FREQ     0x20u  /* R   – mains frequency (50 or 60 Hz)  */
#define REG_CALIBRATION 0x23u  /* R   – 0=running, 1=done              */

/* UART fallback – same physical pins, DimmerLink in factory UART mode:
 *   SDA (GPIO4) = DimmerLink UART TX → configure as ESP UART TX
 *   SCL (GPIO0) = DimmerLink UART RX → configure as ESP UART RX        */
#define DIMMERLINK_UART_NUM     UART_NUM_2
#define DIMMERLINK_UART_BAUD    115200
#define DIMMERLINK_UART_TX_PIN  DIMMERLINK_SCL_PIN   /* GPIO4 */
#define DIMMERLINK_UART_RX_PIN  DIMMERLINK_SDA_PIN   /* GPIO0 */

/* SWITCH_I2C command frame: STX(0x02) + CMD(0x5B) */
static const uint8_t k_switch_i2c_cmd[2] = { 0x02u, 0x5Bu };

/* ── Module state ──────────────────────────────────────────────────────── */

static i2c_master_bus_handle_t s_bus = nullptr;
static i2c_master_dev_handle_t s_dev = nullptr;

/* ── I2C helpers ───────────────────────────────────────────────────────── */

/**
 * Write a single register address then read one byte back.
 * Returns ESP_OK on success.
 */
static esp_err_t read_reg(uint8_t reg, uint8_t *out)
{
    return i2c_master_transmit_receive(s_dev,
                                       &reg, sizeof(reg),
                                       out,  1,
                                       /*timeout_ms=*/50);
}

/* ── UART fallback ─────────────────────────────────────────────────────── */

/**
 * Send the SWITCH_I2C command over UART (factory default mode).
 * Opens UART2 temporarily, sends [02 5B], waits for ACK [00], then closes.
 * After a successful switch the device restarts in I2C mode.
 *
 * @return true  ACK received – device has switched to I2C.
 * @return false No/bad response – device likely not present.
 */
static bool uart_switch_to_i2c(void)
{
    ESP_LOGI(TAG, "DimmerLink: trying UART SWITCH_I2C on GPIO%d/GPIO%d ...",
             DIMMERLINK_UART_TX_PIN, DIMMERLINK_UART_RX_PIN);

    uart_config_t uart_cfg = {};
    uart_cfg.baud_rate           = DIMMERLINK_UART_BAUD;
    uart_cfg.data_bits           = UART_DATA_8_BITS;
    uart_cfg.parity              = UART_PARITY_DISABLE;
    uart_cfg.stop_bits           = UART_STOP_BITS_1;
    uart_cfg.flow_ctrl           = UART_HW_FLOWCTRL_DISABLE;
    uart_cfg.rx_flow_ctrl_thresh = 0;
    uart_cfg.source_clk          = UART_SCLK_DEFAULT;

    esp_err_t err = uart_driver_install(DIMMERLINK_UART_NUM,
                                        /*rx_buf=*/256, /*tx_buf=*/0,
                                        /*queue_size=*/0, /*queue=*/NULL,
                                        /*intr_flags=*/0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return false;
    }

    uart_param_config(DIMMERLINK_UART_NUM, &uart_cfg);
    uart_set_pin(DIMMERLINK_UART_NUM,
                 DIMMERLINK_UART_TX_PIN,
                 DIMMERLINK_UART_RX_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    uart_flush(DIMMERLINK_UART_NUM);
    uart_write_bytes(DIMMERLINK_UART_NUM,
                     reinterpret_cast<const char *>(k_switch_i2c_cmd),
                     sizeof(k_switch_i2c_cmd));

    /* Wait up to 100 ms for the single ACK byte (0x00 = OK) */
    uint8_t ack = 0xFFu;
    int n = uart_read_bytes(DIMMERLINK_UART_NUM, &ack, 1, pdMS_TO_TICKS(100));

    uart_driver_delete(DIMMERLINK_UART_NUM);

    if (n == 1 && ack == 0x00u) {
        ESP_LOGI(TAG, "DimmerLink: SWITCH_I2C ACK received – waiting for I2C boot ...");
        /* Device saves mode to EEPROM and restarts; give it time to come up */
        vTaskDelay(pdMS_TO_TICKS(150));
        return true;
    }

    ESP_LOGW(TAG, "DimmerLink: UART SWITCH_I2C no ACK (n=%d, byte=0x%02X) – device absent?",
             n, ack);
    return false;
}

/* ── Public API ────────────────────────────────────────────────────────── */

bool dimmerlink_probe(void)
{
    /* ── 1. Prepare I2C master bus config (reused on retry) ─────────────── */
    i2c_master_bus_config_t bus_cfg;
    memset(&bus_cfg, 0, sizeof(bus_cfg));
    bus_cfg.i2c_port           = I2C_NUM_0;
    bus_cfg.sda_io_num         = (gpio_num_t)DIMMERLINK_SDA_PIN;
    bus_cfg.scl_io_num         = (gpio_num_t)DIMMERLINK_SCL_PIN;
    bus_cfg.clk_source         = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt  = 7;
    /* External 4.7 kΩ pull-ups are required; do not enable internal pull-ups. */
    bus_cfg.flags.enable_internal_pullup = false;

    /* ── 2. Initialise I2C master bus ───────────────────────────────────── */
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2C master bus: %s", esp_err_to_name(err));
        return false;
    }

    /* ── 3. Probe for device at 0x50 ────────────────────────────────────── */
    err = i2c_master_probe(s_bus, DIMMERLINK_I2C_ADDR, /*timeout_ms=*/200);
    if (err != ESP_OK) {
        /* Not found – could be in factory UART mode. Tear down I2C first,
         * because the UART uses the same physical pins.                    */
        ESP_LOGW(TAG, "DimmerLink not found at 0x%02X via I2C", DIMMERLINK_I2C_ADDR);
        i2c_del_master_bus(s_bus);
        s_bus = nullptr;

        if (!uart_switch_to_i2c()) {
            ESP_LOGW(TAG, "DimmerLink not present (UART fallback also failed)");
            return false;
        }

        /* Re-create I2C bus and retry probe */
        err = i2c_new_master_bus(&bus_cfg, &s_bus);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to re-create I2C bus after UART switch: %s",
                     esp_err_to_name(err));
            return false;
        }

        err = i2c_master_probe(s_bus, DIMMERLINK_I2C_ADDR, /*timeout_ms=*/100);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "DimmerLink still not found after UART SWITCH_I2C");
            i2c_del_master_bus(s_bus);
            s_bus = nullptr;
            return false;
        }

        ESP_LOGI(TAG, "DimmerLink now in I2C mode at 0x%02X", DIMMERLINK_I2C_ADDR);
    } else {
        ESP_LOGI(TAG, "DimmerLink detected at 0x%02X (already in I2C mode)", DIMMERLINK_I2C_ADDR);
    }

    /* ── 4. Register device handle ──────────────────────────────────────── */
    i2c_device_config_t dev_cfg;
    memset(&dev_cfg, 0, sizeof(dev_cfg));
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = DIMMERLINK_I2C_ADDR;
    dev_cfg.scl_speed_hz    = DIMMERLINK_SPEED_HZ;

    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device: %s", esp_err_to_name(err));
        i2c_del_master_bus(s_bus);
        s_bus = nullptr;
        return false;
    }

    /* ── 5. Read status registers ───────────────────────────────────────── */
    uint8_t status  = 0;
    uint8_t error   = 0;
    uint8_t version = 0;
    uint8_t level   = 0;
    uint8_t curve   = 0;
    uint8_t freq    = 0;
    uint8_t cal     = 0;

    bool ok = true;
    ok &= (read_reg(REG_STATUS,      &status)  == ESP_OK);
    ok &= (read_reg(REG_ERROR,       &error)   == ESP_OK);
    ok &= (read_reg(REG_VERSION,     &version) == ESP_OK);
    ok &= (read_reg(REG_DIM0_LEVEL,  &level)   == ESP_OK);
    ok &= (read_reg(REG_DIM0_CURVE,  &curve)   == ESP_OK);
    ok &= (read_reg(REG_AC_FREQ,     &freq)    == ESP_OK);
    ok &= (read_reg(REG_CALIBRATION, &cal)     == ESP_OK);

    if (!ok) {
        ESP_LOGW(TAG, "One or more register reads failed");
    }

    /* ── 6. Decode and log ──────────────────────────────────────────────── */
    const char *curve_str;
    switch (curve) {
        case 0:  curve_str = "LINEAR"; break;
        case 1:  curve_str = "RMS";    break;
        case 2:  curve_str = "LOG";    break;
        default: curve_str = "?";      break;
    }

    const char *error_str;
    switch (error) {
        case 0x00: error_str = "OK";            break;
        case 0xF9: error_str = "ERR_SYNTAX";    break;
        case 0xFC: error_str = "ERR_NOT_READY"; break;
        case 0xFD: error_str = "ERR_INDEX";     break;
        case 0xFE: error_str = "ERR_PARAM";     break;
        default:   error_str = "UNKNOWN";        break;
    }

    ESP_LOGI(TAG, "------- DimmerLink Status --------");
    ESP_LOGI(TAG, "  Ready      : %s", (status & 0x01u) ? "YES" : "NO");
    ESP_LOGI(TAG, "  Error flag : %s", (status & 0x02u) ? "SET" : "clear");
    ESP_LOGI(TAG, "  Error code : 0x%02X (%s)", error, error_str);
    ESP_LOGI(TAG, "  FW version : %u", version);
    ESP_LOGI(TAG, "  Brightness : %u%%", level);
    ESP_LOGI(TAG, "  Curve      : %u (%s)", curve, curve_str);
    ESP_LOGI(TAG, "  AC freq    : %u Hz", freq);
    ESP_LOGI(TAG, "  Calibrated : %s", cal ? "YES" : "in progress");
    ESP_LOGI(TAG, "----------------------------------");

    return true;
}

void dimmerlink_set_level(uint8_t pct)
{
    if (!s_dev) return;   /* probe failed or not called yet */
    if (pct > 100u) pct = 100u;
    uint8_t buf[2] = { REG_DIM0_LEVEL, pct };
    i2c_master_transmit(s_dev, buf, sizeof(buf), /*timeout_ms=*/20);
}
