#pragma once

#include <stdint.h>

typedef void (*gui_action_t)(void);

typedef struct {
    uint16_t x, y;          /* top-left corner of hit region */
    uint16_t w, h;          /* width and height */
    const uint16_t *pressed_img; /* RGB565 patch (w*h pixels), NULL → darken */
    gui_action_t on_press;
} gui_button_t;

void gui_init(uint16_t *fb, uint16_t fb_w, uint16_t fb_h);
void gui_register_button(const gui_button_t *btn);
void gui_handle_touch(uint16_t x, uint16_t y);
