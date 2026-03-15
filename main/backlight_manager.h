#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Callback type for setting backlight brightness (0–100 %). */
typedef void (*backlight_set_fn)(int percent);

/* Initialise the manager and start the scheduling task.
   `set_brightness` will be called whenever the backlight level changes. */
esp_err_t backlight_manager_init(backlight_set_fn set_brightness);

/* Increase / decrease the day-time brightness by one step (10 %, clamped to 5–100 %). */
void backlight_manager_up(void);
void backlight_manager_down(void);

/* Override the schedule: set backlight to 100 % for 60 seconds. */
void backlight_manager_on_button_press(void);

/* Returns true if the backlight is currently on (schedule or button override). */
bool backlight_manager_is_on(void);
