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

/* ---------- Overlay (status left + dTdt right) ---------- */
#define STATUS_FONT_SCALE   2
#define STATUS_CHAR_W       (8 * STATUS_FONT_SCALE)
#define STATUS_CHAR_H       (8 * STATUS_FONT_SCALE)
#define STATUS_LINE_STEP    (STATUS_CHAR_H + 6)   /* 6 px gap between lines */
#define STATUS_LINES        5
#define DTDT_LINES          9
/* Background must cover the taller of the two blocks */
#define OVERLAY_LINES       DTDT_LINES
#define OVERLAY_BLOCK_H     (OVERLAY_LINES * STATUS_CHAR_H + (OVERLAY_LINES - 1) * 6)

/* Bottom of the overlay is 40 px above the screen edge */
#define OVERLAY_Y_TOP       (720 - 40 - OVERLAY_BLOCK_H)

/* Status block (5 lines) bottom-aligns with 40 px from screen edge */
#define STATUS_BLOCK_H      (STATUS_LINES * STATUS_CHAR_H + (STATUS_LINES - 1) * 6)
#define STATUS_Y_TOP        (720 - 40 - STATUS_BLOCK_H)

#define STATUS_X_LEFT       40
#define STATUS_X_RIGHT      680   /* right edge of dTdt block */
/* dTdt block: 8 chars wide (4-char label + space + 3-char value), left-aligned */
#define DTDT_X_LEFT         (STATUS_X_RIGHT - 8 * STATUS_CHAR_W)
#define STATUS_COLOR        0x630C  /* grey ~(96,96,96) */

/* UTF-8 Δ is not in the ASCII font; we repurpose 0x7F (DEL, unused) as Δ */
#define DELTA               "\x7f"

static uint16_t     *fb     = NULL;
static uint16_t      fb_w   = 0;
static uint16_t      fb_h   = 0;
static button_slot_t btns[MAX_BUTTONS];
static int           btn_count = 0;
static gui_action_t  touch_cb  = NULL;

static combustion_controler_info_t last_info;
static bool                        has_info = false;
static dTdt_data_t                 last_dTdt;
static bool                        has_dTdt = false;
static int16_t                     cached_session_max[3];
static uint16_t                   *overlay_bg = NULL; /* saved fireplace bg under overlay block */

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
                    if (px < fb_w && py < fb_h)
                        fb[py * fb_w + px] = color;
                }
            }
        }
    }
}

static void draw_status_line(const char *text, uint16_t cy)
{
    size_t len = strlen(text);
    for (size_t i = 0; i < len; i++)
        draw_char_status((uint16_t)(STATUS_X_LEFT + i * STATUS_CHAR_W), cy, text[i], STATUS_COLOR);
    msync_aligned(fb + cy * fb_w, (size_t)fb_w * STATUS_CHAR_H * sizeof(uint16_t));
}

static void draw_dTdt_line(const char *text, uint16_t cy)
{
    size_t len = strlen(text);
    for (size_t i = 0; i < len; i++)
        draw_char_status((uint16_t)(DTDT_X_LEFT + i * STATUS_CHAR_W), cy, text[i], STATUS_COLOR);
    msync_aligned(fb + cy * fb_w, (size_t)fb_w * STATUS_CHAR_H * sizeof(uint16_t));
}

/* Format a raw int16 (°C × 10) as "[-]INT.FRAC C" into buf. */
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


static void render_overlay(void)
{
    if (!fb) return;

    /* Restore fireplace background to erase any previous text */
    if (overlay_bg) {
        memcpy(fb + OVERLAY_Y_TOP * fb_w, overlay_bg,
               (size_t)fb_w * OVERLAY_BLOCK_H * sizeof(uint16_t));
    }

    char line[32];
    char tmp[16];   /* used by fmt_temp for status block */

    /* ---- Left: status block (5 lines) — bottom-aligned to 40 px from screen edge ---- */
    uint16_t y = STATUS_Y_TOP;

    if (has_info) fmt_temp(tmp, sizeof(tmp), last_info.ext_temp);
    else            snprintf(tmp, sizeof(tmp), "-");
    snprintf(line, sizeof(line), "Zew.: %s", tmp);
    draw_status_line(line, y);
    y += STATUS_LINE_STEP;

    if (has_info) fmt_temp(tmp, sizeof(tmp), last_info.exhaust_temp);
    else            snprintf(tmp, sizeof(tmp), "-");
    snprintf(line, sizeof(line), "Spaliny: %s", tmp);
    draw_status_line(line, y);
    y += STATUS_LINE_STEP;

    if (has_info) snprintf(line, sizeof(line), "Nawiew: %d%%", last_info.vent_pct);
    else            snprintf(line, sizeof(line), "Nawiew: -");
    draw_status_line(line, y);
    y += STATUS_LINE_STEP;

    if (has_info) snprintf(line, sizeof(line), "Kominek: %d%%", last_info.fire_pct);
    else            snprintf(line, sizeof(line), "Kominek: -");
    draw_status_line(line, y);
    y += STATUS_LINE_STEP;

    if (has_info) {
        static const char *const state_str[] = {
            "wylaczony", "rozpalanie", "praca",
            "ochrona", "konczenie", "wygaszanie"
        };
        uint8_t st = last_info.combustion_state;
        const char *label = (st < 6) ? state_str[st] : "?";
        snprintf(line, sizeof(line), "Stan: %s", label);
    } else {
        snprintf(line, sizeof(line), "Stan: -");
    }
    draw_status_line(line, y);

    /* ---- Right: dTdt block (9 lines, right-justified) ---- */
    y = OVERLAY_Y_TOP;

    /* dT/dt rows: Δ10, Δ20, Δ30 — space-padded to 4-char label so all rows align */
    if (has_dTdt) snprintf(line, sizeof(line), " " DELTA "10 %3d", (int)last_dTdt.dTdt_10s);
    else          snprintf(line, sizeof(line), " " DELTA "10   -");
    draw_dTdt_line(line, y);
    y += STATUS_LINE_STEP;

    if (has_dTdt) snprintf(line, sizeof(line), " " DELTA "20 %3d", (int)last_dTdt.dTdt_20s);
    else          snprintf(line, sizeof(line), " " DELTA "20   -");
    draw_dTdt_line(line, y);
    y += STATUS_LINE_STEP;

    if (has_dTdt) snprintf(line, sizeof(line), " " DELTA "30 %3d", (int)last_dTdt.dTdt_30s);
    else          snprintf(line, sizeof(line), " " DELTA "30   -");
    draw_dTdt_line(line, y);
    y += STATUS_LINE_STEP;

    /* Sliding max rows: SΔ10, SΔ20, SΔ30 */
    if (has_dTdt) snprintf(line, sizeof(line), "S" DELTA "10 %3d", (int)last_dTdt.sliding_max_10s);
    else          snprintf(line, sizeof(line), "S" DELTA "10   -");
    draw_dTdt_line(line, y);
    y += STATUS_LINE_STEP;

    if (has_dTdt) snprintf(line, sizeof(line), "S" DELTA "20 %3d", (int)last_dTdt.sliding_max_20s);
    else          snprintf(line, sizeof(line), "S" DELTA "20   -");
    draw_dTdt_line(line, y);
    y += STATUS_LINE_STEP;

    if (has_dTdt) snprintf(line, sizeof(line), "S" DELTA "30 %3d", (int)last_dTdt.sliding_max_30s);
    else          snprintf(line, sizeof(line), "S" DELTA "30   -");
    draw_dTdt_line(line, y);
    y += STATUS_LINE_STEP;

    /* Session max rows: MΔ10, MΔ20, MΔ30 */
    if (has_dTdt) snprintf(line, sizeof(line), "M" DELTA "10 %3d", (int)cached_session_max[0]);
    else          snprintf(line, sizeof(line), "M" DELTA "10   -");
    draw_dTdt_line(line, y);
    y += STATUS_LINE_STEP;

    if (has_dTdt) snprintf(line, sizeof(line), "M" DELTA "20 %3d", (int)cached_session_max[1]);
    else          snprintf(line, sizeof(line), "M" DELTA "20   -");
    draw_dTdt_line(line, y);
    y += STATUS_LINE_STEP;

    if (has_dTdt) snprintf(line, sizeof(line), "M" DELTA "30 %3d", (int)cached_session_max[2]);
    else          snprintf(line, sizeof(line), "M" DELTA "30   -");
    draw_dTdt_line(line, y);
}

void gui_on_controller_info(const combustion_controler_info_t *info)
{
    last_info = *info;
    has_info  = true;
    render_overlay();
    ESP_LOGD(TAG, "Controller info updated");
}

void gui_on_dTdt(const dTdt_data_t *data, const int16_t session_max[3])
{
    last_dTdt = *data;
    has_dTdt  = true;
    cached_session_max[0] = session_max[0];
    cached_session_max[1] = session_max[1];
    cached_session_max[2] = session_max[2];
    render_overlay();
}

void gui_redraw_status(void)
{
    render_overlay();
}

/* ------------------------------------------------------------------ */

void gui_init(uint16_t *buf, uint16_t w, uint16_t h)
{
    fb    = buf;
    fb_w  = w;
    fb_h  = h;
    btn_count = 0;

    /* Save the fireplace background under the overlay block so render_overlay
       can restore it before each redraw (prevents stale character pixels). */
    overlay_bg = malloc((size_t)fb_w * OVERLAY_BLOCK_H * sizeof(uint16_t));
    if (overlay_bg) {
        memcpy(overlay_bg, fb + OVERLAY_Y_TOP * fb_w,
               (size_t)fb_w * OVERLAY_BLOCK_H * sizeof(uint16_t));
    }

    render_overlay();  /* draw "-" placeholders until first responses */
}

void gui_register_button(const gui_button_t *btn)
{
    if (btn_count >= MAX_BUTTONS) {
        ESP_LOGE(TAG, "Too many buttons");
        return;
    }
    button_slot_t *slot = &btns[btn_count];
    slot->def = *btn;

    size_t pixels = btn->w * btn->h;
    slot->backup = malloc(pixels * sizeof(uint16_t));
    if (!slot->backup) {
        ESP_LOGE(TAG, "OOM for button backup");
        return;
    }

    /* Save original pixels row-by-row */
    for (int row = 0; row < btn->h; row++) {
        const uint16_t *src = fb + (btn->y + row) * fb_w + btn->x;
        memcpy(slot->backup + row * btn->w, src, btn->w * sizeof(uint16_t));
    }

    btn_count++;
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

static void msync_full(void)
{
    msync_aligned(fb, (size_t)fb_w * fb_h * sizeof(uint16_t));
}

/* ------------------------------------------------------------------ */
/*  Full-screen rendering (error overlay, fireplace restore)            */
/* ------------------------------------------------------------------ */
#define ERR_FONT_SCALE  3
#define ERR_CHAR_W      (8 * ERR_FONT_SCALE)
#define ERR_CHAR_H      (8 * ERR_FONT_SCALE)
#define ERR_LINE_GAP    8

static uint16_t make_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

static void draw_char_3x(uint16_t cx, uint16_t cy, char c, uint16_t color)
{
    if ((uint8_t)c < 0x20 || (uint8_t)c > 0x7F) c = '?';
    const uint8_t *glyph = font8x8[(uint8_t)(c - 0x20)];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (!(bits & (1u << col))) continue;
            for (int sy = 0; sy < ERR_FONT_SCALE; sy++) {
                for (int sx = 0; sx < ERR_FONT_SCALE; sx++) {
                    uint16_t px = cx + col * ERR_FONT_SCALE + sx;
                    uint16_t py = cy + row * ERR_FONT_SCALE + sy;
                    if (px < fb_w && py < fb_h)
                        fb[py * fb_w + px] = color;
                }
            }
        }
    }
}

static void draw_text_centered_3x(const char *text, uint16_t cy, uint16_t color)
{
    size_t len = strlen(text);
    uint16_t total_w = (uint16_t)(len * ERR_CHAR_W);
    uint16_t cx = (fb_w > total_w) ? (fb_w - total_w) / 2 : 0;
    for (size_t i = 0; i < len; i++) {
        draw_char_3x((uint16_t)(cx + i * ERR_CHAR_W), cy, text[i], color);
    }
}

void gui_show_error(const char *l1, const char *l2, const char *l3)
{
    if (!fb) return;
    uint16_t bg = make_rgb565(18, 12, 12);
    for (uint32_t i = 0; i < (uint32_t)fb_w * fb_h; i++) {
        fb[i] = bg;
    }
    uint16_t red = make_rgb565(220, 55, 55);
    uint16_t total_h = 3 * ERR_CHAR_H + 2 * ERR_LINE_GAP;
    uint16_t y0 = (fb_h > total_h) ? (fb_h - total_h) / 2 : 0;
    draw_text_centered_3x(l1, y0,                                    red);
    draw_text_centered_3x(l2, y0 + ERR_CHAR_H + ERR_LINE_GAP,       red);
    draw_text_centered_3x(l3, y0 + 2 * (ERR_CHAR_H + ERR_LINE_GAP), red);
    msync_full();
    ESP_LOGW(TAG, "Error screen displayed");
}

void gui_show_fireplace(const uint8_t *img, size_t sz)
{
    if (!fb) return;
    memcpy(fb, img, sz);
    msync_full();
    ESP_LOGI(TAG, "Fireplace image restored");
}

static void blit_region(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                         const uint16_t *pixels)
{
    for (int row = 0; row < h; row++) {
        uint16_t *dst = fb + (y + row) * fb_w + x;
        memcpy(dst, pixels + row * w, w * sizeof(uint16_t));
    }
    msync_aligned(fb + y * fb_w, (size_t)fb_w * h * sizeof(uint16_t));
}

static void darken_region(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    for (int row = 0; row < h; row++) {
        uint16_t *p = fb + (y + row) * fb_w + x;
        for (int col = 0; col < w; col++) {
            /* Halve each channel: R5 >> 1, G6 >> 1, B5 >> 1 */
            uint16_t px = p[col];
            uint16_t r = (px >> 11) & 0x1F;
            uint16_t g = (px >>  5) & 0x3F;
            uint16_t b =  px        & 0x1F;
            p[col] = (uint16_t)(((r >> 1) << 11) | ((g >> 1) << 5) | (b >> 1));
        }
    }
    msync_aligned(fb + y * fb_w, (size_t)fb_w * h * sizeof(uint16_t));
}

void gui_set_touch_callback(gui_action_t cb)
{
    touch_cb = cb;
}

void gui_handle_touch(uint16_t x, uint16_t y)
{
    if (touch_cb) touch_cb();

    for (int i = 0; i < btn_count; i++) {
        const gui_button_t *btn = &btns[i].def;
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
        blit_region(btn->x, btn->y, btn->w, btn->h, btns[i].backup);

        if (btn->on_press) {
            btn->on_press();
        }

        return; /* only one button per touch */
    }

    ESP_LOGI(TAG, "Touch miss at (%d,%d)", x, y);
}
