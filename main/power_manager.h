#pragma once

#include <stdbool.h>
#include "esp_err.h"

/**
 * Initialise the power manager.
 * Configures dynamic CPU frequency scaling (light sleep disabled — MIPI DSI
 * requires a continuous pixel clock). Acquires the CPU_FREQ_MAX lock
 * immediately because the display is active at boot.
 */
esp_err_t power_manager_init(void);

/**
 * Notify the power manager that the display has become active or idle.
 *
 * active = true  → acquire CPU_FREQ_MAX lock (display on, needs full speed)
 * active = false → release lock (display backlight off, CPU may scale down)
 *
 * Call this from the backlight setter whenever brightness crosses zero.
 */
void power_manager_set_active(bool active);
