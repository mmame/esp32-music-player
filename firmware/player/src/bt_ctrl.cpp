/**
 * @file bt_ctrl.cpp
 * @brief JAB5 BLE-disconnect active push/pull driver.
 *
 * GPIO46 is permanently configured as OUTPUT (never high-Z):
 *   Disabled: 600 ms OUTPUT HIGH → 1400 ms OUTPUT LOW  [repeating, 2 s period]
 *   Enabled : OUTPUT LOW permanently
 */

#include "bt_ctrl.h"
#include "pins.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "bt_ctrl";

static volatile bool s_enabled = false;

/* ── Pulse task ─────────────────────────────────────────────────────────────── */

static void bt_ctrl_task(void *arg)
{
    for (;;) {
        if (s_enabled) {
            /* BT enabled: hold LOW, check state every 100 ms */
            gpio_set_level((gpio_num_t)BT_CTRL_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
        } else {
            /* BT disabled: 600 ms HIGH pulse (active drive) */
            gpio_set_level((gpio_num_t)BT_CTRL_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(600));
            /* 1400 ms LOW (active drive, not high-Z) */
            gpio_set_level((gpio_num_t)BT_CTRL_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(1400));
        }
    }
}

/* ── Public API ─────────────────────────────────────────────────────────────── */

void bt_ctrl_init(void)
{
    s_enabled = false;

    gpio_reset_pin((gpio_num_t)BT_CTRL_PIN);
    gpio_set_direction((gpio_num_t)BT_CTRL_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)BT_CTRL_PIN, 0);

    xTaskCreatePinnedToCore(bt_ctrl_task, "bt_ctrl",
                            2048, nullptr, 2, nullptr, 0);

    ESP_LOGI(TAG, "init: GPIO%d OUTPUT, state=disabled (pulsing)", BT_CTRL_PIN);
}

void bt_ctrl_set_enabled(bool enabled)
{
    if (s_enabled == enabled) return;
    s_enabled = enabled;
    ESP_LOGI(TAG, "BT %s", enabled ? "enabled (LOW)" : "disabled (pulsing)");
}

bool bt_ctrl_is_enabled(void)
{
    return s_enabled;
}
