#pragma once
#include "esp_err.h"
#include <stdint.h>

/**
 * Called from a FreeRTOS task whenever a finger is detected on screen.
 * x and y are in display pixels (0–719).
 */
typedef void (*touch_event_cb_t)(uint16_t x, uint16_t y);

/**
 * Initialise the GT911 capacitive touch controller over I2C,
 * then start a background polling task.
 *
 * @param on_touch  Callback invoked with (x, y) on every touch event.
 *                  Pass NULL if you don't need the callback.
 */
esp_err_t touch_init(touch_event_cb_t on_touch);
