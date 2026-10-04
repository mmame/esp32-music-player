/**
 * @file sunton_esp32_2432s032.c
 * @brief Board support for the Sunton ESP32-2432S032C: 320x240 ST7789 (SPI) + GT911 touch.
 *        Provides the same board_*() API as sunton_esp32_8048s050c.c.
 */

#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch_gt911.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "lvgl.h"

#include "display_board.h"
#include "sunton_esp32_2432s032.h"

static esp_timer_handle_t lvgl_tick_timer_handle = NULL;
static lv_indev_t *indev_touchpad = NULL;

void board_backlight_init(void)
{
    ledc_timer_config_t ledc_timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num       = BOARD_BACKLIGHT_LEDC_TIMER,
        .freq_hz         = 4000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    ledc_channel_config_t ledc_channel = {
        .gpio_num   = BOARD_LCD_PIN_BCKL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = BOARD_BACKLIGHT_LEDC_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = BOARD_BACKLIGHT_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, BOARD_BACKLIGHT_LEDC_CHANNEL, 255));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, BOARD_BACKLIGHT_LEDC_CHANNEL));
}

/* Panel setup from the vendor demo (Arduino_GFX st7789_init_operations). ESP-IDF's ST7789
 * driver only sends SLPOUT/MADCTL/COLMOD/RAMCTRL, so the porch/gate/VCOM/power/gamma
 * registers have to be programmed here. */
static void st7789_vendor_init(esp_lcd_panel_io_handle_t io)
{
    static const uint8_t gamma_p[14] = {0xF0, 0x09, 0x13, 0x12, 0x12, 0x2B, 0x3C, 0x44, 0x4B, 0x1B, 0x18, 0x17, 0x1D, 0x21};
    static const uint8_t gamma_n[14] = {0xF0, 0x09, 0x13, 0x0C, 0x0D, 0x27, 0x3B, 0x44, 0x4D, 0x0B, 0x17, 0x17, 0x1D, 0x21};
    static const uint8_t porch[5]    = {0x0C, 0x0C, 0x00, 0x33, 0x33};
    static const uint8_t d0[2]       = {0xA4, 0xA1};

    esp_lcd_panel_io_tx_param(io, 0xB2, porch, sizeof(porch));
    esp_lcd_panel_io_tx_param(io, 0xB7, (uint8_t[]){0x35}, 1);   /* gate control */
    esp_lcd_panel_io_tx_param(io, 0xBB, (uint8_t[]){0x19}, 1);   /* VCOM */
    esp_lcd_panel_io_tx_param(io, 0xC0, (uint8_t[]){0x2C}, 1);   /* LCM control */
    esp_lcd_panel_io_tx_param(io, 0xC2, (uint8_t[]){0x01}, 1);   /* VDV/VRH enable */
    esp_lcd_panel_io_tx_param(io, 0xC3, (uint8_t[]){0x12}, 1);   /* VRH */
    esp_lcd_panel_io_tx_param(io, 0xC4, (uint8_t[]){0x20}, 1);   /* VDV */
    esp_lcd_panel_io_tx_param(io, 0xC6, (uint8_t[]){0x0F}, 1);   /* frame rate */
    esp_lcd_panel_io_tx_param(io, 0xD0, d0, sizeof(d0));         /* power control */
    esp_lcd_panel_io_tx_param(io, 0xE0, gamma_p, sizeof(gamma_p));
    esp_lcd_panel_io_tx_param(io, 0xE1, gamma_n, sizeof(gamma_n));
    esp_lcd_panel_io_tx_param(io, 0x13, NULL, 0);                /* NORON */
    vTaskDelay(pdMS_TO_TICKS(10));
}

/* SPI transfer finished -> LVGL may reuse the draw buffer */
static bool lcd_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_display_flush_ready((lv_display_t *)user_ctx);
    return false;
}

static void lvgl_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)lv_display_get_user_data(disp);

    static int flush_log_count = 0;   /* trace the first flushes (visible in the display boot log) */
    if (flush_log_count < 8) {
        flush_log_count++;
        ESP_LOGI("board", "LVGL flush #%d: x %d..%d y %d..%d", flush_log_count,
                 (int)area->x1, (int)area->x2, (int)area->y1, (int)area->y2);
    }

    /* ST7789 expects big-endian RGB565 */
    lv_draw_sw_rgb565_swap(px_map, lv_area_get_width(area) * lv_area_get_height(area));
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
}

static void lvgl_port_task(void *arg)
{
#ifdef CONFIG_LV_OS_FREERTOS
    lv_draw_init();
#endif
    uint32_t task_delay_ms;
    while (1)
    {
        task_delay_ms = lv_timer_handler();
        if (task_delay_ms > CONFIG_LVGL_TASK_MAX_DELAY_MS) task_delay_ms = CONFIG_LVGL_TASK_MAX_DELAY_MS;
        else if (task_delay_ms < CONFIG_LVGL_TASK_MIN_DELAY_MS) task_delay_ms = CONFIG_LVGL_TASK_MIN_DELAY_MS;
        vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
    }
}

static void lvgl_tick(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static const char *TAG = "board";

/** Fill the whole panel with one (byte-swapped RGB565) colour, using `buf` (DMA, buf_size bytes) as source. */
static void panel_fill(esp_lcd_panel_handle_t panel, void *buf, size_t buf_size, uint16_t color_swapped)
{
    uint16_t *px = (uint16_t *)buf;
    for (size_t i = 0; i < buf_size / sizeof(uint16_t); i++) px[i] = color_swapped;
    for (int y = 0; y < BOARD_LCD_HEIGHT; y += BOARD_LVGL_BUF_LINES) {
        esp_lcd_panel_draw_bitmap(panel, 0, y, BOARD_LCD_WIDTH, y + BOARD_LVGL_BUF_LINES, buf);
        vTaskDelay(pdMS_TO_TICKS(15));   /* wait for the DMA transfer (the flush callback is not registered yet) */
    }
}

lv_display_t *board_lcd_init(void)
{
    ESP_LOGI(TAG, "2432S032: SPI%d MOSI=%d SCLK=%d CS=%d DC=%d mode 3 @ %d Hz", (int)BOARD_LCD_SPI_HOST + 1,
             (int)BOARD_LCD_PIN_MOSI, (int)BOARD_LCD_PIN_SCLK, (int)BOARD_LCD_PIN_CS, (int)BOARD_LCD_PIN_DC,
             (int)BOARD_LCD_SPI_HZ);
    const spi_bus_config_t bus_cfg = {
        .mosi_io_num     = BOARD_LCD_PIN_MOSI,
        .miso_io_num     = -1,
        .sclk_io_num     = BOARD_LCD_PIN_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = BOARD_LCD_WIDTH * BOARD_LVGL_BUF_LINES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(BOARD_LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = BOARD_LCD_PIN_DC,
        .cs_gpio_num       = BOARD_LCD_PIN_CS,
        .pclk_hz           = BOARD_LCD_SPI_HZ,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .spi_mode          = 3,   /* the vendor demo forces SPI_MODE3 on ESP32 for this ST7789 */
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_SPI_HOST, &io_cfg, &io));

    esp_lcd_panel_handle_t panel = NULL;
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BOARD_LCD_PIN_RST,
        .rgb_ele_order  = BOARD_LCD_BGR ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_LOGI(TAG, "ST7789 reset + init done");
    st7789_vendor_init(io);
    ESP_LOGI(TAG, "ST7789 vendor registers sent");
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, BOARD_LCD_INVERT));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, BOARD_LCD_SWAP_XY));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, BOARD_LCD_MIRROR_X, BOARD_LCD_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    vTaskDelay(pdMS_TO_TICKS(120));   /* let the panel settle before the first frame */

    ESP_LOGI(TAG, "panel on (swap_xy=%d mirror=%d,%d invert=%d)", BOARD_LCD_SWAP_XY, BOARD_LCD_MIRROR_X,
             BOARD_LCD_MIRROR_Y, BOARD_LCD_INVERT);
    lv_init();
    lv_display_t *disp = lv_display_create(BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT);
    lv_display_set_user_data(disp, panel);
    lv_display_set_flush_cb(disp, lvgl_disp_flush);

    const size_t buf_size = BOARD_LCD_WIDTH * BOARD_LVGL_BUF_LINES * sizeof(uint16_t);
    void *buf1 = heap_caps_malloc(buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *buf2 = heap_caps_malloc(buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_ERROR_CHECK((buf1 && buf2) ? ESP_OK : ESP_ERR_NO_MEM);
    lv_display_set_buffers(disp, buf1, buf2, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    ESP_LOGI(TAG, "LVGL draw buffers: 2 x %u bytes (DMA, internal)", (unsigned)buf_size);
    /* Always start from a defined (black) frame, like the vendor demo does before LVGL starts. */
    panel_fill(panel, buf1, buf_size, 0x0000);
#if BOARD_LCD_BOOT_TEST
    ESP_LOGI(TAG, "colour test: red / green / blue");
    {
        static const uint16_t colors[3] = {0x00F8, 0xE007, 0x1F00};   /* RGB565 red/green/blue, byte-swapped */
        for (int c = 0; c < 3; c++) {
            panel_fill(panel, buf1, buf_size, colors[c]);
            vTaskDelay(pdMS_TO_TICKS(400));
        }
        panel_fill(panel, buf1, buf_size, 0x0000);
    }
#endif

    const esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = lcd_trans_done };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io, &cbs, disp));

    const esp_timer_create_args_t tick_args = { .callback = &lvgl_tick, .name = "lvgl_tick" };
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &lvgl_tick_timer_handle));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer_handle, LVGL_TICK_PERIOD_MS * 1000));

    xTaskCreate(lvgl_port_task, "lvgl_port_task", CONFIG_LVGL_TASK_STACK_SIZE * 1024, NULL,
                CONFIG_LVGL_TASK_PRIORITY, NULL);
    return disp;
}

i2c_master_bus_handle_t board_i2c_master(void)
{
    i2c_master_bus_handle_t bus = NULL;
    const i2c_master_bus_config_t cfg = {
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .i2c_port          = -1,
        .scl_io_num        = BOARD_TOUCH_PIN_SCL,
        .sda_io_num        = BOARD_TOUCH_PIN_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = 1,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &bus));
    return bus;
}

/* GT911 coordinate range (probed from its config registers in board_touch_init) */
static uint16_t gt911_x_max = BOARD_TOUCH_NATIVE_X_MAX;
static uint16_t gt911_y_max = BOARD_TOUCH_NATIVE_Y_MAX;

/* Raw GT911 (240x320 portrait) -> 320x240 landscape screen, as in the vendor demo */
static void process_coordinates(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength,
                                uint8_t *point_num, uint8_t max_point_num)
{
    for (uint8_t i = 0; i < *point_num; i++)
    {
        uint32_t raw_x = x[i], raw_y = y[i];
        if (raw_y > gt911_y_max) raw_y = gt911_y_max;
        if (raw_x > gt911_x_max) raw_x = gt911_x_max;
        uint32_t sx = BOARD_LCD_WIDTH - raw_y * BOARD_LCD_WIDTH / gt911_y_max;
        uint32_t sy = raw_x * BOARD_LCD_HEIGHT / gt911_x_max;
        x[i] = (uint16_t)(sx >= BOARD_LCD_WIDTH ? BOARD_LCD_WIDTH - 1 : sx);
        y[i] = (uint16_t)(sy >= BOARD_LCD_HEIGHT ? BOARD_LCD_HEIGHT - 1 : sy);
    }
}

static void touchpad_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    esp_lcd_touch_handle_t tp = (esp_lcd_touch_handle_t)lv_indev_get_user_data(indev);
    esp_lcd_touch_point_data_t pt;
    uint8_t cnt = 0;

    esp_lcd_touch_read_data(tp);
    esp_lcd_touch_get_data(tp, &pt, &cnt, 1);
    if (cnt > 0) {
        data->point.x = pt.x;
        data->point.y = pt.y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

void board_touch_init(i2c_master_bus_handle_t i2c_master)
{
    esp_lcd_panel_io_handle_t tp_io = NULL;
    const esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
        .on_color_trans_done = NULL,
        .user_ctx = NULL,
        .control_phase_bytes = 1,
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 16,
        .lcd_param_bits = 0,
        .flags = { .dc_low_on_data = 0, .disable_control_phase = 1 },
        .scl_speed_hz = 400000,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_master, &io_cfg, &tp_io));

    esp_lcd_touch_io_gt911_config_t gt911_cfg = { .dev_addr = io_cfg.dev_addr };
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_TOUCH_NATIVE_X_MAX,
        .y_max = BOARD_TOUCH_NATIVE_Y_MAX,
        .rst_gpio_num = BOARD_TOUCH_PIN_RST,
        .int_gpio_num = BOARD_TOUCH_PIN_INT,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
        .driver_data = &gt911_cfg,
        .process_coordinates = process_coordinates,
        .interrupt_callback = NULL,
    };
    esp_lcd_touch_handle_t tp = NULL;
    /* A missing/unresponsive touch controller must not take the whole UI down. */
    esp_err_t err = esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &tp);
    if (err != ESP_OK || !tp) {
        ESP_LOGE("board", "GT911 init failed (%s) - continuing without touch", esp_err_to_name(err));
        return;
    }

    /* Probe the coordinate range configured inside the GT911 (0x8048/49 X max, 0x804A/4B Y max). */
    uint8_t cfg[5] = {0};
    if (esp_lcd_panel_io_rx_param(tp_io, 0x8047, cfg, sizeof(cfg)) == ESP_OK) {
        uint16_t xm = cfg[1] | (cfg[2] << 8);
        uint16_t ym = cfg[3] | (cfg[4] << 8);
        if (xm >= 64 && xm <= 4096 && ym >= 64 && ym <= 4096) {
            gt911_x_max = xm;
            gt911_y_max = ym;
        }
        ESP_LOGI("board", "GT911 range %ux%u", gt911_x_max, gt911_y_max);
    }

    lv_lock();   /* the LVGL task is already running: don't race with its timer / indev lists */
    indev_touchpad = lv_indev_create();
    lv_indev_set_type(indev_touchpad, LV_INDEV_TYPE_POINTER);
    lv_indev_set_user_data(indev_touchpad, tp);
    lv_indev_set_read_cb(indev_touchpad, touchpad_read);
    lv_unlock();
}
