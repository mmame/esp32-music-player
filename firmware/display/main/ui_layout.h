/**
 * @file ui_layout.h
 * @brief UI layout variant, fixed per firmware build (CONFIG_DISPLAY_BOARD_*).
 */
#pragma once

#include <stdbool.h>
#include "sdkconfig.h"
#include "lvgl.h"

/** Compact layout for the 320x240 board; full layout for 800x480. */
static inline bool ui_is_compact(void)
{
#if CONFIG_DISPLAY_BOARD_2432S032
    return true;
#else
    return false;
#endif
}

/** Song-name font (player title). */
static inline const lv_font_t *ui_font_song(void)
{
    /* Not 36/40: those built-in fonts have no Latin-1 diacritics (ä, ö, ü, ... show as squares). */
    return &lv_font_montserrat_28;
}

/** Song-list row font. */
static inline const lv_font_t *ui_font_list(void)
{
    return &lv_font_montserrat_28;
}

/** Elapsed / total time font. */
static inline const lv_font_t *ui_font_time(void)
{
    return ui_is_compact() ? &lv_font_montserrat_36 : &lv_font_montserrat_28;
}
