# Two distinct firmwares. The board option in Kconfig.projbuild is tied to the IDF target
# (esp32 = 2432S032, esp32s3 = 8048S050C); CMake keys on the target because component
# requirements are expanded before sdkconfig values (CONFIG_*) are available.
if(IDF_TARGET STREQUAL "esp32")
    set(board_srcs "sunton_esp32_2432s032.c")
    set(board_reqs esp_lcd_touch_gt911)
else()
    set(board_srcs "sunton_esp32_8048s050c.c")
    set(board_reqs esp_lcd_touch_gt911)
endif()

idf_component_register(SRCS "ui_player.c" "ui_songlist.c" "uart_comm.c" "main.c" ${board_srcs}
                    INCLUDE_DIRS "."
                    REQUIRES lvgl esp_lcd esp_timer driver esp_app_format ${board_reqs})
