#include "gui.h"
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_cache.h"
#include "esp_log.h"

#define MAX_BUTTONS     8
#define PRESS_HOLD_MS   150
#define CACHE_LINE_SIZE 64

static const char *TAG = "gui";

typedef struct {
    gui_button_t def;
    uint16_t    *backup; /* saved pixels for restore after animation */
} button_slot_t;

static uint16_t     *s_fb     = NULL;
static uint16_t      s_fb_w   = 0;
static uint16_t      s_fb_h   = 0;
static button_slot_t s_btns[MAX_BUTTONS];
static int           s_btn_count = 0;

void gui_init(uint16_t *fb, uint16_t fb_w, uint16_t fb_h)
{
    s_fb    = fb;
    s_fb_w  = fb_w;
    s_fb_h  = fb_h;
    s_btn_count = 0;
}

void gui_register_button(const gui_button_t *btn)
{
    if (s_btn_count >= MAX_BUTTONS) {
        ESP_LOGE(TAG, "Too many buttons");
        return;
    }
    button_slot_t *slot = &s_btns[s_btn_count];
    slot->def = *btn;

    size_t pixels = btn->w * btn->h;
    slot->backup = malloc(pixels * sizeof(uint16_t));
    if (!slot->backup) {
        ESP_LOGE(TAG, "OOM for button backup");
        return;
    }

    /* Save original pixels row-by-row */
    for (int row = 0; row < btn->h; row++) {
        const uint16_t *src = s_fb + (btn->y + row) * s_fb_w + btn->x;
        memcpy(slot->backup + row * btn->w, src, btn->w * sizeof(uint16_t));
    }

    s_btn_count++;
    ESP_LOGI(TAG, "Button registered at (%d,%d) %dx%d", btn->x, btn->y, btn->w, btn->h);
}

/* msync with start/size aligned to cache line boundaries */
static void msync_aligned(void *ptr, size_t size)
{
    uintptr_t start = (uintptr_t)ptr & ~(uintptr_t)(CACHE_LINE_SIZE - 1);
    uintptr_t end   = ((uintptr_t)ptr + size + CACHE_LINE_SIZE - 1)
                      & ~(uintptr_t)(CACHE_LINE_SIZE - 1);
    esp_cache_msync((void *)start, end - start, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

static void blit_region(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                         const uint16_t *pixels)
{
    for (int row = 0; row < h; row++) {
        uint16_t *dst = s_fb + (y + row) * s_fb_w + x;
        memcpy(dst, pixels + row * w, w * sizeof(uint16_t));
    }
    msync_aligned(s_fb + y * s_fb_w, (size_t)s_fb_w * h * sizeof(uint16_t));
}

static void darken_region(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    for (int row = 0; row < h; row++) {
        uint16_t *p = s_fb + (y + row) * s_fb_w + x;
        for (int col = 0; col < w; col++) {
            /* Halve each channel: R5 >> 1, G6 >> 1, B5 >> 1 */
            uint16_t px = p[col];
            uint16_t r = (px >> 11) & 0x1F;
            uint16_t g = (px >>  5) & 0x3F;
            uint16_t b =  px        & 0x1F;
            p[col] = (uint16_t)(((r >> 1) << 11) | ((g >> 1) << 5) | (b >> 1));
        }
    }
    msync_aligned(s_fb + y * s_fb_w, (size_t)s_fb_w * h * sizeof(uint16_t));
}

void gui_handle_touch(uint16_t x, uint16_t y)
{
    for (int i = 0; i < s_btn_count; i++) {
        const gui_button_t *btn = &s_btns[i].def;
        if (x < btn->x || x >= btn->x + btn->w) continue;
        if (y < btn->y || y >= btn->y + btn->h) continue;

        ESP_LOGI(TAG, "Button %d hit at (%d,%d)", i, x, y);

        /* Show pressed state */
        if (btn->pressed_img) {
            blit_region(btn->x, btn->y, btn->w, btn->h, btn->pressed_img);
        } else {
            darken_region(btn->x, btn->y, btn->w, btn->h);
        }

        vTaskDelay(pdMS_TO_TICKS(PRESS_HOLD_MS));

        /* Restore original pixels */
        blit_region(btn->x, btn->y, btn->w, btn->h, s_btns[i].backup);

        if (btn->on_press) {
            btn->on_press();
        }

        return; /* only one button per touch */
    }

    ESP_LOGI(TAG, "Touch miss at (%d,%d)", x, y);
}
