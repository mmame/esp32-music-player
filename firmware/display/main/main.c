#include "freertos/FreeRTOS.h"
#include "driver/i2c_master.h"
#include "lvgl.h"

#include "sunton_esp32_8048s050c.h"
#include "uart_comm.h"
#include "ui_songlist.h"
#include "ui_player.h"

void app_main(void)
{
    sunton_esp32s3_backlight_init();

    lv_display_t *disp = sunton_esp32s3_lcd_init();
    (void)disp;

    i2c_master_bus_handle_t i2c_master = sunton_esp32s3_i2c_master();
    sunton_esp32s3_touch_init(i2c_master);

    /* Create both screens while no other task can preempt us.
     * ui_songlist_create() loads the songlist as the active screen.
     * ui_player_create()  creates the player screen without loading it. */
    lv_lock();
    ui_songlist_create();
    ui_player_create();
    lv_unlock();

    /* Start UART communication layer (task pinned to Core 0) */
    uart_comm_init();
}