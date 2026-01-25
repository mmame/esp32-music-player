#ifndef ABOUT_UI_H
#define ABOUT_UI_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void about_ui_init(void);
void about_show(void);
void about_hide(void);
lv_obj_t * about_get_screen(void);

#ifdef __cplusplus
}
#endif

#endif // ABOUT_UI_H
