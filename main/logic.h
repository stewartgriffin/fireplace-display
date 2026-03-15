#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/**
 * Initialise the logic module.
 * Starts a 60 s time-sync timer and a 5 s status poll timer (gated on
 * backlight being on). Must be called after gui_init and backlight_manager_init.
 *
 * @param fireplace_img  Pointer to the embedded fireplace image in flash.
 * @param fireplace_sz   Size in bytes (w * h * 2).
 */
esp_err_t logic_init(const uint8_t *fireplace_img, size_t fireplace_sz);

/**
 * Call from the on_time_received callback whenever a TIME_RESPONSE arrives.
 * Clears the comm-error flag and restores the fireplace image if it was replaced.
 */
void logic_on_time_synced(void);

/**
 * Call BEFORE gui_handle_touch on every touch event.
 *
 * In standby mode (display completely off) the first tap activates the backlight
 * at 100 % for 60 s and does NOT pass through to the GUI buttons.
 * In evening mode (display dimly on via seasonal schedule) all taps pass
 * through immediately — no touch is consumed.
 *
 * @return true  Touch was consumed (standby wake) — do NOT call gui_handle_touch.
 * @return false Touch passes through normally — call gui_handle_touch.
 */
bool logic_on_touch(void);
