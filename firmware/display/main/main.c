#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_system.h"
#include "driver/i2c_master.h"
#include "lvgl.h"

#include "display_board.h"
#include "uart_comm.h"
#include "ui_songlist.h"
#include "ui_player.h"

static const char *TAG = "display";

void app_main(void)
{
    ESP_LOGI(TAG, "app_main: free heap %u", (unsigned)esp_get_free_heap_size());
    board_backlight_init();

    lv_display_t *disp = board_lcd_init();
    (void)disp;
    ESP_LOGI(TAG, "LCD + LVGL initialised (%dx%d), free heap %u",
             (int)lv_display_get_horizontal_resolution(disp), (int)lv_display_get_vertical_resolution(disp),
             (unsigned)esp_get_free_heap_size());

    i2c_master_bus_handle_t i2c_master = board_i2c_master();
    board_touch_init(i2c_master);
    ESP_LOGI(TAG, "touch initialised");

    /* Create both screens while no other task can preempt us.
     * ui_songlist_create() loads the songlist as the active screen.
     * ui_player_create()  creates the player screen without loading it. */
    lv_lock();
    ui_songlist_create();
    ui_player_create();
    lv_unlock();
    ESP_LOGI(TAG, "UI created, free heap %u - starting UART link", (unsigned)esp_get_free_heap_size());

    /* Start UART communication layer (task pinned to Core 0) */
    uart_comm_init();
}