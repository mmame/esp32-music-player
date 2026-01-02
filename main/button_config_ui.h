#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

// Initialize the button config UI
void button_config_ui_init(void);

// Show/hide the button config screen
void button_config_show(void);
void button_config_hide(void);

// Get the button config screen
lv_obj_t * button_config_get_screen(void);

// Get current ADC values
uint16_t button_config_get_adc_value(void);

// Check if a button is pressed based on ADC ranges
int button_config_get_button_index(void);

#ifdef __cplusplus
}
#endif
