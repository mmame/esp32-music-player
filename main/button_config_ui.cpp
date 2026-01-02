#include "button_config_ui.h"
#include "audio_player_ui.h"
#include "file_manager_ui.h"
#include "wifi_config_ui.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

static const char *TAG = "ButtonConfig";

// ADC Configuration - Using GPIO2 (ADC1 Channel 1)
#define BUTTON_ADC_UNIT         ADC_UNIT_1
#define BUTTON_ADC_CHANNEL      ADC_CHANNEL_1  // GPIO2
#define BUTTON_ADC_ATTEN        ADC_ATTEN_DB_12
#define BUTTON_SAMPLE_COUNT     10
#define BUTTON_DEBOUNCE_MS      50

#define NUM_BUTTONS 6
#define BUTTON_TOLERANCE 100  // Tolerance for button range (±50 on each side)

// Button configuration structure
typedef struct {
    uint16_t adc_min;
    uint16_t adc_max;
    const char* action_name;
    bool configured;
} button_config_t;

// Default button thresholds (can be overridden by learning)
static button_config_t button_configs[NUM_BUTTONS] = {
    {350,  850, "Play",       true},
    {750,  1250, "Pause",      true},
    {1150, 1650, "Play/Pause", true},
    {1550, 2050, "Previous",   true},
    {1950, 2450, "Next",       true},
    {2350, 4095, "Stop",       true}
};

// UI elements
static lv_obj_t *button_config_screen = NULL;
static lv_obj_t *adc_value_label = NULL;
static lv_obj_t *action_list = NULL;
static lv_obj_t *back_btn = NULL;

// List item elements for each button
static lv_obj_t *list_items[NUM_BUTTONS];
static lv_obj_t *range_labels[NUM_BUTTONS];
static lv_obj_t *learn_buttons[NUM_BUTTONS];
static lv_obj_t *clear_buttons[NUM_BUTTONS];

// ADC handle
static adc_oneshot_unit_handle_t adc_handle = NULL;
static bool adc_initialized = false;

// Button state
static int last_button_pressed = -1;
static uint32_t last_button_time = 0;

// Learning state
static int learning_button_index = -1;  // Which button we're learning (-1 = none)

// Forward declarations
static void init_adc(void);
static void button_scan_task(void *arg);
static void update_ui_task(void *arg);

// Event handlers
static void back_btn_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        button_config_hide();
        audio_player_show();
    }
}

// Initialize ADC for button scanning
static void init_adc(void)
{
    if (adc_initialized) {
        return;
    }

    // Configure GPIO2 as input with pull-down
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << GPIO_NUM_2);
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "GPIO2 configured for ADC");
    vTaskDelay(pdMS_TO_TICKS(100)); // Let GPIO settle

    // Configure ADC
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = BUTTON_ADC_UNIT,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle));

    // Configure ADC channel
    adc_oneshot_chan_cfg_t config = {
        .atten = BUTTON_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, BUTTON_ADC_CHANNEL, &config));

    adc_initialized = true;
    ESP_LOGI(TAG, "ADC initialized on GPIO2 (ADC1_CH1)");
}

// Get current ADC value (averaged)
uint16_t button_config_get_adc_value(void)
{
    if (!adc_initialized) {
        init_adc();
    }

    uint32_t adc_sum = 0;
    int adc_raw;
    int valid_samples = 0;

    // Take multiple samples and average
    for (int i = 0; i < BUTTON_SAMPLE_COUNT; i++) {
        esp_err_t ret = adc_oneshot_read(adc_handle, BUTTON_ADC_CHANNEL, &adc_raw);
        if (ret == ESP_OK) {
            adc_sum += adc_raw;
            valid_samples++;
        } else if (ret == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "ADC read timeout (attempt %d)", i);
            vTaskDelay(pdMS_TO_TICKS(5));
        } else {
            ESP_LOGE(TAG, "ADC read error: %s", esp_err_to_name(ret));
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (valid_samples == 0) {
        ESP_LOGW(TAG, "No valid ADC samples");
        return 0;
    }

    return (uint16_t)(adc_sum / valid_samples);
}

// Determine which button is pressed based on ADC value
int button_config_get_button_index(void)
{
    uint16_t adc_value = button_config_get_adc_value();

    // Check if ADC value is too low (no button pressed)
    if (adc_value < 300) {
        return -1; // No button pressed
    }

    // Check each configured button
    for (int i = 0; i < NUM_BUTTONS; i++) {
        if (button_configs[i].configured) {
            if (adc_value >= button_configs[i].adc_min && adc_value <= button_configs[i].adc_max) {
                return i;
            }
        }
    }

    return -1; // No matching button
}

// Check if an ADC value overlaps with existing button ranges
static bool check_overlap(uint16_t adc_min, uint16_t adc_max, int exclude_index)
{
    for (int i = 0; i < NUM_BUTTONS; i++) {
        if (i == exclude_index || !button_configs[i].configured) {
            continue;
        }
        
        // Check for overlap
        if ((adc_min >= button_configs[i].adc_min && adc_min <= button_configs[i].adc_max) ||
            (adc_max >= button_configs[i].adc_min && adc_max <= button_configs[i].adc_max) ||
            (adc_min <= button_configs[i].adc_min && adc_max >= button_configs[i].adc_max)) {
            return true; // Overlap detected
        }
    }
    return false;
}

// Assign ADC value to a button
static bool assign_button_value(int button_index, uint16_t adc_value)
{
    if (button_index < 0 || button_index >= NUM_BUTTONS) {
        return false;
    }

    // Calculate range with tolerance
    uint16_t adc_min = (adc_value > BUTTON_TOLERANCE) ? (adc_value - BUTTON_TOLERANCE) : 0;
    uint16_t adc_max = (adc_value + BUTTON_TOLERANCE > 4095) ? 4095 : (adc_value + BUTTON_TOLERANCE);

    // Check for overlap
    if (check_overlap(adc_min, adc_max, button_index)) {
        ESP_LOGE(TAG, "Button %d: Range %d-%d overlaps with existing button", button_index, adc_min, adc_max);
        
        // Show error on the learn button temporarily
        lv_lock();
        if (learn_buttons[button_index] && lv_obj_is_valid(learn_buttons[button_index])) {
            lv_obj_t *btn_label = lv_obj_get_child(learn_buttons[button_index], 0);
            if (btn_label) {
                lv_label_set_text(btn_label, "OVERLAP!");
                lv_obj_set_style_bg_color(learn_buttons[button_index], lv_color_hex(0xFF0000), 0);
            }
        }
        lv_unlock();
        
        // Reset after 1 second
        vTaskDelay(pdMS_TO_TICKS(1000));
        lv_lock();
        if (learn_buttons[button_index] && lv_obj_is_valid(learn_buttons[button_index])) {
            lv_obj_t *btn_label = lv_obj_get_child(learn_buttons[button_index], 0);
            if (btn_label) {
                lv_label_set_text(btn_label, "Learn");
                lv_obj_set_style_bg_color(learn_buttons[button_index], lv_color_hex(0x0066CC), 0);
            }
        }
        lv_unlock();
        
        return false;
    }

    // Assign the values
    button_configs[button_index].adc_min = adc_min;
    button_configs[button_index].adc_max = adc_max;
    button_configs[button_index].configured = true;

    ESP_LOGI(TAG, "Button %d learned: ADC %d (range %d-%d)", button_index, adc_value, adc_min, adc_max);
    
    return true;
}

// Clear button configuration
static void clear_button_config(int button_index)
{
    if (button_index < 0 || button_index >= NUM_BUTTONS) {
        return;
    }
    
    button_configs[button_index].configured = false;
    button_configs[button_index].adc_min = 0;
    button_configs[button_index].adc_max = 0;
    
    ESP_LOGI(TAG, "Button %d cleared", button_index);
}

// Button scanning task
static void button_scan_task(void *arg)
{
    while (1) {
        uint16_t adc_value = button_config_get_adc_value();
        int button_index = button_config_get_button_index();
        uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;

        // Handle learning mode
        if (learning_button_index >= 0) {
            // Wait for a button press (ADC value above threshold)
            if (adc_value > 300) {
                // Assign this value to the learning button
                if (assign_button_value(learning_button_index, adc_value)) {
                    // Learning successful - update button color
                    lv_lock();
                    if (learn_buttons[learning_button_index] && lv_obj_is_valid(learn_buttons[learning_button_index])) {
                        lv_obj_t *btn_label = lv_obj_get_child(learn_buttons[learning_button_index], 0);
                        if (btn_label) {
                            lv_label_set_text(btn_label, "OK!");
                            lv_obj_set_style_bg_color(learn_buttons[learning_button_index], lv_color_hex(0x00AA00), 0);
                        }
                    }
                    lv_unlock();
                    
                    vTaskDelay(pdMS_TO_TICKS(500)); // Show success briefly
                    
                    // Reset learn button
                    lv_lock();
                    if (learn_buttons[learning_button_index] && lv_obj_is_valid(learn_buttons[learning_button_index])) {
                        lv_obj_t *btn_label = lv_obj_get_child(learn_buttons[learning_button_index], 0);
                        if (btn_label) {
                            lv_label_set_text(btn_label, "Learn");
                            lv_obj_set_style_bg_color(learn_buttons[learning_button_index], lv_color_hex(0x0066CC), 0);
                        }
                    }
                    lv_unlock();
                    
                    learning_button_index = -1; // Done learning
                }
            }
        } else {
            // Normal mode - handle button actions
                        switch (button_index) {
                            case 0:
                                // Play
                                audio_player_resume();
                                break;
                            case 1:
                                // Pause
                                audio_player_pause();
                                break;
                            case 2:
                                // Play/Pause toggle
                                if (audio_player_is_playing()) {
                                    audio_player_pause();
                                } else {
                                    audio_player_resume();
                                }
                                break;
                            case 3:
                                // Previous track
                                audio_player_previous();
                                break;
                            case 4:
                                // Next track
                                audio_player_next();
                                break;
                            case 5:
                                // Stop
                                audio_player_stop();
                                break;
                        }
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10)); // Check every 10ms
    }
}

// UI update task
static void update_ui_task(void *arg)
{
    while (1) {
        if (button_config_screen != NULL && lv_obj_is_valid(button_config_screen)) {
            lv_lock();
            
            // Update ADC value
            if (adc_value_label && lv_obj_is_valid(adc_value_label)) {
                uint16_t adc = button_config_get_adc_value();
                lv_label_set_text_fmt(adc_value_label, "Current ADC: %d", adc);
            }

            // Update range labels for all buttons
            for (int i = 0; i < NUM_BUTTONS; i++) {
                if (range_labels[i] && lv_obj_is_valid(range_labels[i])) {
                    if (button_configs[i].configured) {
                        lv_label_set_text_fmt(range_labels[i], "Range: %d - %d", 
                                            button_configs[i].adc_min, button_configs[i].adc_max);
                        lv_obj_set_style_text_color(range_labels[i], lv_color_hex(0x00FF00), 0);
                    } else {
                        lv_label_set_text(range_labels[i], "Not configured");
                        lv_obj_set_style_text_color(range_labels[i], lv_color_hex(0x888888), 0);
                    }
                }
            }
            
            lv_unlock();
        }
        
        vTaskDelay(pdMS_TO_TICKS(100)); // Update UI every 100ms
    }
}

// Create the button config UI
static void create_button_config_ui(void)
{
    button_config_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(button_config_screen, lv_color_hex(0x000000), LV_PART_MAIN);

    // Title
    lv_obj_t *title = lv_label_create(button_config_screen);
    lv_label_set_text(title, "Button Configuration");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x00FF00), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // Current ADC value
    adc_value_label = lv_label_create(button_config_screen);
    lv_label_set_text(adc_value_label, "Current ADC: 0");
    lv_obj_set_style_text_color(adc_value_label, lv_color_hex(0xFFFF00), 0);
    lv_obj_set_style_text_font(adc_value_label, &lv_font_montserrat_20, 0);
    lv_obj_align(adc_value_label, LV_ALIGN_TOP_MID, 0, 45);

    // Create scrollable list container
    action_list = lv_obj_create(button_config_screen);
    lv_obj_set_size(action_list, 760, 340);
    lv_obj_align(action_list, LV_ALIGN_CENTER, 0, 15);
    lv_obj_set_style_bg_color(action_list, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_color(action_list, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_border_width(action_list, 2, 0);
    lv_obj_set_flex_flow(action_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(action_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(action_list, 5, 0);
    lv_obj_set_style_pad_all(action_list, 10, 0);

    // Create list items for each button
    for (int i = 0; i < NUM_BUTTONS; i++) {
        // List item container
        list_items[i] = lv_obj_create(action_list);
        lv_obj_set_size(list_items[i], 720, 50);
        lv_obj_set_style_bg_color(list_items[i], lv_color_hex(0x2a2a2a), 0);
        lv_obj_set_style_border_width(list_items[i], 1, 0);
        lv_obj_set_style_border_color(list_items[i], lv_color_hex(0x444444), 0);
        lv_obj_clear_flag(list_items[i], LV_OBJ_FLAG_SCROLLABLE);

        // Action name label (left)
        lv_obj_t *action_label = lv_label_create(list_items[i]);
        lv_label_set_text(action_label, button_configs[i].action_name);
        lv_obj_set_style_text_color(action_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(action_label, &lv_font_montserrat_14, 0);
        lv_obj_align(action_label, LV_ALIGN_LEFT_MID, 10, 0);

        // Range label (center-left)
        range_labels[i] = lv_label_create(list_items[i]);
        char range_str[32];
        if (button_configs[i].configured) {
            snprintf(range_str, sizeof(range_str), "%d - %d", button_configs[i].adc_min, button_configs[i].adc_max);
            lv_obj_set_style_text_color(range_labels[i], lv_color_hex(0x00FF00), 0);
        } else {
            snprintf(range_str, sizeof(range_str), "Not configured");
            lv_obj_set_style_text_color(range_labels[i], lv_color_hex(0x888888), 0);
        }
        lv_label_set_text(range_labels[i], range_str);
        lv_obj_align(range_labels[i], LV_ALIGN_LEFT_MID, 200, 0);

        // Learn button
        learn_buttons[i] = lv_btn_create(list_items[i]);
        lv_obj_set_size(learn_buttons[i], 100, 35);
        lv_obj_align(learn_buttons[i], LV_ALIGN_RIGHT_MID, -120, 0);
        lv_obj_set_style_bg_color(learn_buttons[i], lv_color_hex(0x0066CC), 0);
        
        lv_obj_t *learn_label = lv_label_create(learn_buttons[i]);
        lv_label_set_text(learn_label, "Learn");
        lv_obj_center(learn_label);
        
        lv_obj_add_event_cb(learn_buttons[i], [](lv_event_t * e) {
            lv_event_code_t code = lv_event_get_code(e);
            if (code == LV_EVENT_CLICKED) {
                int btn_idx = (int)(intptr_t)lv_event_get_user_data(e);
                learning_button_index = btn_idx;
                
                lv_lock();
                // Update all learn buttons - highlight the one being learned
                for (int j = 0; j < NUM_BUTTONS; j++) {
                    if (learn_buttons[j] && lv_obj_is_valid(learn_buttons[j])) {
                        if (j == btn_idx) {
                            lv_obj_set_style_bg_color(learn_buttons[j], lv_color_hex(0xFF8800), 0);
                            lv_obj_t *lbl = lv_obj_get_child(learn_buttons[j], 0);
                            if (lbl) lv_label_set_text(lbl, "Press...");
                        } else {
                            lv_obj_set_style_bg_color(learn_buttons[j], lv_color_hex(0x0066CC), 0);
                        }
                    }
                }
                lv_unlock();
                
                ESP_LOGI(TAG, "Learning button %d - press physical button now", btn_idx);
            }
        }, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        // Clear button
        clear_buttons[i] = lv_btn_create(list_items[i]);
        lv_obj_set_size(clear_buttons[i], 100, 35);
        lv_obj_align(clear_buttons[i], LV_ALIGN_RIGHT_MID, -10, 0);
        lv_obj_set_style_bg_color(clear_buttons[i], lv_color_hex(0xAA0000), 0);
        
        lv_obj_t *clear_label = lv_label_create(clear_buttons[i]);
        lv_label_set_text(clear_label, "Clear");
        lv_obj_center(clear_label);
        
        lv_obj_add_event_cb(clear_buttons[i], [](lv_event_t * e) {
            lv_event_code_t code = lv_event_get_code(e);
            if (code == LV_EVENT_CLICKED) {
                int btn_idx = (int)(intptr_t)lv_event_get_user_data(e);
                clear_button_config(btn_idx);
                ESP_LOGI(TAG, "Cleared button %d configuration", btn_idx);
            }
        }, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }

    // Back button
    back_btn = lv_btn_create(button_config_screen);
    lv_obj_set_size(back_btn, 150, 50);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_add_event_cb(back_btn, back_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);
}

// Initialize button config UI
void button_config_ui_init(void)
{
    // Initialize ADC
    init_adc();

    // Create UI
    create_button_config_ui();

    // Start button scanning task
    xTaskCreate(button_scan_task, "button_scan", 4096, NULL, 5, NULL);

    // Start UI update task
    xTaskCreate(update_ui_task, "button_ui_update", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "Button config UI initialized");
}

// Show button config screen
void button_config_show(void)
{
    if (button_config_screen) {
        lv_screen_load(button_config_screen);
        ESP_LOGI(TAG, "Button config screen shown");
    }
}

// Hide button config screen (just switch away)
void button_config_hide(void)
{
    // Screen will be hidden when another screen loads
    ESP_LOGI(TAG, "Button config screen hidden");
}

// Get button config screen
lv_obj_t * button_config_get_screen(void)
{
    return button_config_screen;
}

// External declarations for functions used by button actions
extern "C" void audio_player_hide(void);
