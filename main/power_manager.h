#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/**
 * Initialise the power manager.
 * Starts a 60-second periodic timer that calls comm_request_time().
 * If a poll gets no response before the next poll, the error screen is shown.
 *
 * @param fb            Frame buffer in PSRAM (same pointer passed to gui_init).
 * @param fb_w, fb_h    Frame buffer dimensions.
 * @param fireplace_img Pointer to the embedded fireplace image in flash.
 * @param fireplace_sz  Size in bytes (fb_w * fb_h * 2).
 */
esp_err_t power_manager_init(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                              const uint8_t *fireplace_img, size_t fireplace_sz);

/**
 * Call this from the on_time_received callback in main.c.
 * Clears the comm-error flag and restores the fireplace image if it was replaced.
 */
void power_manager_on_time_synced(void);

/**
 * Call this BEFORE gui_handle_touch on every touch event.
 *
 * While the display is dim (no recent touch), the first tap activates the
 * backlight at 100% for 60 s but does NOT pass through to the GUI buttons.
 *
 * @return true  Touch was consumed — do NOT call gui_handle_touch.
 * @return false Touch passes through normally — call gui_handle_touch.
 */
bool power_manager_on_touch(void);
