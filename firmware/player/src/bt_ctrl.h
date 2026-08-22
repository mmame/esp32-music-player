/**
 * @file bt_ctrl.h
 * @brief JAB5 BLE-disconnect active push/pull driver (GPIO46).
 *
 * Disabled state: 600 ms OUTPUT HIGH then 1400 ms OUTPUT LOW (repeating, 2 s period).
 * Enabled state:  OUTPUT LOW permanently.
 * Starts in disabled state at power-on.
 */
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Initialise GPIO46 and start the pulse task. Call once from app_main. */
void bt_ctrl_init(void);

/** Enable or disable the BLE module. Thread-safe. */
void bt_ctrl_set_enabled(bool enabled);

/** Returns the current enable state. */
bool bt_ctrl_is_enabled(void);

#ifdef __cplusplus
}
#endif
