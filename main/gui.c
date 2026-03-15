#include "gui.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_cache.h"
#include "esp_log.h"
#include "font8x8.h"

#define MAX_BUTTONS     8
#define PRESS_HOLD_MS   150
#define CACHE_LINE_SIZE 64

static const char *TAG = "gui";

typedef struct {
    gui_button_t def;
    uint16_t    *backup; /* saved pixels for restore after animation */
} button_slot_t;

/* ---------- Status overlay ---------- */
#define STATUS_FONT_SCALE   2
#define STATUS_CHAR_W       (8 * STATUS_FONT_SCALE)
#define STATUS_CHAR_H       (8 * STATUS_FONT_SCALE)
#define STATUS_LINE_STEP    (STATUS_CHAR_H + 6)   /* 6 px gap between lines */
#define STATUS_LINES        5
#define STATUS_BLOCK_H      (STATUS_LINES * STATUS_CHAR_H + (STATUS_LINES - 1) * 6)

/* Bottom of the status block is 40 px above the screen edge */
#define STATUS_Y_TOP        (720 - 40 - STATUS_BLOCK_H)

#define STATUS_COLOR        0x630C  /* grey ~(96,96,96) */

static uint16_t     *s_fb     = NULL;
static uint16_t      s_fb_w   = 0;
static uint16_t      s_fb_h   = 0;
static button_slot_t s_btns[MAX_BUTTONS];
static int           s_btn_count = 0;
static gui_action_t  s_touch_cb  = NULL;

static combustion_controler_info_t s_last_info;
static bool                        s_has_info = false;
static uint16_t                   *s_status_bg = NULL; /* saved fireplace bg under status block */

/* forward declaration — defined below with the other blit helpers */
static void msync_aligned(void *ptr, size_t size);

/* ------------------------------------------------------------------ */
/*  Status overlay rendering                                            */
/* ------------------------------------------------------------------ */
static void draw_char_status(uint16_t cx, uint16_t cy, char c, uint16_t color)
{
    if ((uint8_t)c < 0x20 || (uint8_t)c > 0x7F) c = '?';
    const uint8_t *glyph = font8x8[(uint8_t)(c - 0x20)];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (!(bits & (1u << col))) continue;
            for (int sy = 0; sy < STATUS_FONT_SCALE; sy++) {
                for (int sx = 0; sx < STATUS_FONT_SCALE; sx++) {
                    uint16_t px = cx + col * STATUS_FONT_SCALE + sx;
                    uint16_t py = cy + row * STATUS_FONT_SCALE + sy;
                    if (px < s_fb_w && py < s_fb_h)
                        s_fb[py * s_fb_w + px] = color;
                }
            }
        }
    }
}

#define STATUS_X_LEFT   40

static void draw_status_line(const char *text, uint16_t cy)
{
    size_t len = strlen(text);
    for (size_t i = 0; i < len; i++)
        draw_char_status((uint16_t)(STATUS_X_LEFT + i * STATUS_CHAR_W), cy, text[i], STATUS_COLOR);
    msync_aligned(s_fb + cy * s_fb_w, (size_t)s_fb_w * STATUS_CHAR_H * sizeof(uint16_t));
}

/* Format a raw int16 (°C × 10) as "[-]INT.FRAC" into buf. */
static void fmt_temp(char *buf, size_t buf_len, int16_t raw)
{
    int t = (int)raw;
    int abs_t = (t < 0) ? -t : t;
    int i = abs_t / 10;
    int f = abs_t % 10;
    if (t < 0)
        snprintf(buf, buf_len, "-%d.%d C", i, f);
    else
        snprintf(buf, buf_len, "%d.%d C", i, f);
}

static void render_status(void)
{
    if (!s_fb) return;

    /* Restore fireplace background to erase any previous text */
    if (s_status_bg) {
        memcpy(s_fb + STATUS_Y_TOP * s_fb_w, s_status_bg,
               (size_t)s_fb_w * STATUS_BLOCK_H * sizeof(uint16_t));
    }

    char line[32];
    char tmp[16];
    uint16_t y = STATUS_Y_TOP;

    if (s_has_info) fmt_temp(tmp, sizeof(tmp), s_last_info.ext_temp);
    else            snprintf(tmp, sizeof(tmp), "-");
    snprintf(line, sizeof(line), "Zew.: %s", tmp);
    draw_status_line(line, y);
    y += STATUS_LINE_STEP;

    if (s_has_info) fmt_temp(tmp, sizeof(tmp), s_last_info.exhaust_temp);
    else            snprintf(tmp, sizeof(tmp), "-");
    snprintf(line, sizeof(line), "Spaliny: %s", tmp);
    draw_status_line(line, y);
    y += STATUS_LINE_STEP;

    if (s_has_info) snprintf(line, sizeof(line), "Nawiew: %d%%", s_last_info.vent_pct);
    else            snprintf(line, sizeof(line), "Nawiew: -");
    draw_status_line(line, y);
    y += STATUS_LINE_STEP;

    if (s_has_info) snprintf(line, sizeof(line), "Kominek: %d%%", s_last_info.fire_pct);
    else            snprintf(line, sizeof(line), "Kominek: -");
    draw_status_line(line, y);
    y += STATUS_LINE_STEP;

    if (s_has_info) {
        static const char *const state_str[] = {
            "wylaczony", "rozpalanie", "praca",
            "ochrona", "konczenie", "wygaszanie"
        };
        uint8_t st = s_last_info.combustion_state;
        const char *label = (st < 6) ? state_str[st] : "?";
        snprintf(line, sizeof(line), "Stan: %s", label);
    } else {
        snprintf(line, sizeof(line), "Stan: -");
    }
    draw_status_line(line, y);
}

void gui_on_controller_info(const combustion_controler_info_t *info)
{
    s_last_info = *info;
    s_has_info  = true;
    render_status();
    ESP_LOGD(TAG, "Controller info updated");
}

void gui_redraw_status(void)
{
    render_status();
}

/* ------------------------------------------------------------------ */

void gui_init(uint16_t *fb, uint16_t fb_w, uint16_t fb_h)
{
    s_fb    = fb;
    s_fb_w  = fb_w;
    s_fb_h  = fb_h;
    s_btn_count = 0;

    /* Save the fireplace background under the status block so render_status
       can restore it before each redraw (prevents stale character pixels). */
    s_status_bg = malloc((size_t)fb_w * STATUS_BLOCK_H * sizeof(uint16_t));
    if (s_status_bg) {
        memcpy(s_status_bg, fb + STATUS_Y_TOP * fb_w,
               (size_t)fb_w * STATUS_BLOCK_H * sizeof(uint16_t));
    }

    render_status();  /* draw "-" placeholders until first STATUS_RESPONSE */
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

void gui_set_touch_callback(gui_action_t cb)
{
    s_touch_cb = cb;
}

void gui_handle_touch(uint16_t x, uint16_t y)
{
    if (s_touch_cb) s_touch_cb();

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
