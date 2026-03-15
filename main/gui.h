#pragma once

#include <stdint.h>
#include "comm.h"

typedef void (*gui_action_t)(void);

typedef struct {
    uint16_t x, y;          /* top-left corner of hit region */
    uint16_t w, h;          /* width and height */
    const uint16_t *pressed_img; /* RGB565 patch (w*h pixels), NULL → darken */
    gui_action_t on_press;
} gui_button_t;

void gui_init(uint16_t *fb, uint16_t fb_w, uint16_t fb_h);
void gui_register_button(const gui_button_t *btn);
/* Called on every touch event, before button hit-testing. Pass NULL to clear. */
void gui_set_touch_callback(gui_action_t cb);
void gui_handle_touch(uint16_t x, uint16_t y);

/* Store and render the latest controller info onto the framebuffer.
   Signature matches comm_status_cb_t — wire directly via comm_set_status_cb(). */
void gui_on_controller_info(const combustion_controler_info_t *info);

/* Re-render the last received controller info.
   Call after restoring the background image to avoid stale display. */
void gui_redraw_status(void);
