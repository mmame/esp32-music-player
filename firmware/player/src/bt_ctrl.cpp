/**
 * @file bt_ctrl.cpp
 * @brief JAB5 BLE-disconnect open-collector driver.
 *
 * GPIO46 is driven as an open-emitter output:
 *   Disabled: 300 ms OUTPUT-HIGH → 1700 ms INPUT (high-Z)  [repeating, 2 s period]
 *   Enabled : INPUT (high-Z) permanently
 *
 * The transition between states is handled by a lightweight FreeRTOS task.
 * s_enabled is volatile so the task sees changes immediately without needing
 * a mutex (single-writer, single-reader, bool assignment is atomic on Xtensa).
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
    /* Initial state: input / high-Z */
    gpio_set_direction((gpio_num_t)BT_CTRL_PIN, GPIO_MODE_INPUT);

    for (;;) {
        if (s_enabled) {
            /* BT enabled: hold pin as input, check state every 100 ms */
            gpio_set_direction((gpio_num_t)BT_CTRL_PIN, GPIO_MODE_INPUT);
            vTaskDelay(pdMS_TO_TICKS(100));
        } else {
            /* BT disabled: 300 ms HIGH pulse */
            gpio_set_level((gpio_num_t)BT_CTRL_PIN, 1);
            gpio_set_direction((gpio_num_t)BT_CTRL_PIN, GPIO_MODE_OUTPUT);
            vTaskDelay(pdMS_TO_TICKS(300));
            /* 1700 ms high-Z */
            gpio_set_direction((gpio_num_t)BT_CTRL_PIN, GPIO_MODE_INPUT);
            vTaskDelay(pdMS_TO_TICKS(1700));
        }
    }
}

/* ── Public API ─────────────────────────────────────────────────────────────── */

void bt_ctrl_init(void)
{
    s_enabled = false;

    /* Reset any previous configuration (e.g. ROM boot strapping state) */
    gpio_reset_pin((gpio_num_t)BT_CTRL_PIN);
    gpio_set_direction((gpio_num_t)BT_CTRL_PIN, GPIO_MODE_INPUT);

    xTaskCreatePinnedToCore(bt_ctrl_task, "bt_ctrl",
                            2048, nullptr, 2, nullptr, 0);

    ESP_LOGI(TAG, "init: GPIO%d, state=disabled (pulsing)", BT_CTRL_PIN);
}

void bt_ctrl_set_enabled(bool enabled)
{
    if (s_enabled == enabled) return;
    s_enabled = enabled;
    ESP_LOGI(TAG, "BT %s", enabled ? "enabled" : "disabled");
}

bool bt_ctrl_is_enabled(void)
{
    return s_enabled;
}
