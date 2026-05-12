idf_component_register(SRCS "ui_player.c" "ui_songlist.c" "uart_comm.c" "main.c" "sunton_esp32_8048s050c.c"
                    INCLUDE_DIRS "."
                    REQUIRES lvgl esp_lcd esp_lcd_touch_gt911 esp_timer driver)