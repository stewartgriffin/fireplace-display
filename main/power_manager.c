#include "power_manager.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "comm.h"
#include "gui.h"
#include "backlight_manager.h"

static const char *TAG = "power_mgr";

/* 60-second time-sync poll period (microseconds) */
#define POLL_PERIOD_US          (60LL * 1000000LL)
/* 5-second status poll period (microseconds) */
#define STATUS_POLL_PERIOD_US   (5LL * 1000000LL)

/* Awake window duration — must match BUTTON_OVERRIDE_MS in backlight_manager.c */
#define AWAKE_PERIOD_US   (60LL * 1000000LL)

#define CACHE_LINE_SIZE   64

/* ---------- Frame buffer ---------- */
static uint16_t     *s_fb          = NULL;
static uint16_t      s_fb_w        = 0;
static uint16_t      s_fb_h        = 0;
static const uint8_t *s_fireplace  = NULL;
static size_t         s_fire_sz    = 0;

/* ---------- State (all guarded by s_mutex) ---------- */
static SemaphoreHandle_t s_mutex;
static int64_t  s_awake_until_us = 0;   /* 0 = dim; >now = awake   */
static bool     s_sync_pending   = false;/* set on poll, cleared on sync */
static bool     s_error_shown    = false;

#include "font8x8.h"

/* ------------------------------------------------------------------ */
/*  Text rendering — 3× scaled, centred                                */
/* ------------------------------------------------------------------ */
#define FONT_SCALE  3
#define CHAR_W      (8 * FONT_SCALE)   /* 24 px */
#define CHAR_H      (8 * FONT_SCALE)   /* 24 px */
#define LINE_GAP    8                  /* pixels between lines */

static uint16_t make_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

static void draw_char(uint16_t cx, uint16_t cy, char c, uint16_t color)
{
    if ((uint8_t)c < 0x20 || (uint8_t)c > 0x7F) c = '?';
    const uint8_t *glyph = font8x8[(uint8_t)(c - 0x20)];

    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (1u << col)) {   /* bit-0 = leftmost */
                for (int sy = 0; sy < FONT_SCALE; sy++) {
                    for (int sx = 0; sx < FONT_SCALE; sx++) {
                        uint16_t px = cx + col * FONT_SCALE + sx;
                        uint16_t py = cy + row * FONT_SCALE + sy;
                        if (px < s_fb_w && py < s_fb_h) {
                            s_fb[py * s_fb_w + px] = color;
                        }
                    }
                }
            }
        }
    }
}

static void draw_text_centered(const char *text, uint16_t cy, uint16_t color)
{
    size_t len = strlen(text);
    uint16_t total_w = (uint16_t)(len * CHAR_W);
    uint16_t cx = (s_fb_w > total_w) ? (s_fb_w - total_w) / 2 : 0;
    for (size_t i = 0; i < len; i++) {
        draw_char((uint16_t)(cx + i * CHAR_W), cy, text[i], color);
    }
}

/* ------------------------------------------------------------------ */
/*  Frame-buffer helpers                                                */
/* ------------------------------------------------------------------ */
static void msync_fb(void)
{
    size_t size = (size_t)s_fb_w * s_fb_h * sizeof(uint16_t);
    uintptr_t start = (uintptr_t)s_fb & ~(uintptr_t)(CACHE_LINE_SIZE - 1);
    uintptr_t end   = ((uintptr_t)s_fb + size + CACHE_LINE_SIZE - 1)
                      & ~(uintptr_t)(CACHE_LINE_SIZE - 1);
    esp_cache_msync((void *)start, end - start, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

static void show_error(void)
{
    /* Dark background */
    uint16_t bg = make_rgb565(18, 12, 12);
    for (uint32_t i = 0; i < (uint32_t)s_fb_w * s_fb_h; i++) {
        s_fb[i] = bg;
    }

    /* Three centred lines of error text */
    uint16_t red = make_rgb565(220, 55, 55);
    uint16_t total_h = 3 * CHAR_H + 2 * LINE_GAP;
    uint16_t y0 = (s_fb_h > total_h) ? (s_fb_h - total_h) / 2 : 0;

    draw_text_centered("Komunikacja ze",       y0,                         red);
    draw_text_centered("sterownikiem",          y0 + CHAR_H + LINE_GAP,    red);
    draw_text_centered("przerwana",             y0 + 2*(CHAR_H + LINE_GAP),red);

    msync_fb();
    ESP_LOGW(TAG, "Comm error screen displayed");
}

static void show_fireplace(void)
{
    memcpy(s_fb, s_fireplace, s_fire_sz);
    msync_fb();
    ESP_LOGI(TAG, "Fireplace image restored");
}

/* ------------------------------------------------------------------ */
/*  Periodic time-poll timer                                            */
/* ------------------------------------------------------------------ */
static void poll_timer_cb(void *arg)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool was_pending  = s_sync_pending;
    bool error_before = s_error_shown;
    s_sync_pending = true;
    xSemaphoreGive(s_mutex);

    if (was_pending && !error_before) {
        /* Previous poll went unanswered — show error */
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_error_shown = true;
        xSemaphoreGive(s_mutex);
        show_error();
    }

    comm_request_time();
    ESP_LOGD(TAG, "Time poll sent");
}

static void status_poll_cb(void *arg)
{
    if (backlight_manager_is_on()) {
        comm_request_status();
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */
esp_err_t power_manager_init(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                              const uint8_t *fireplace_img, size_t fireplace_sz)
{
    s_fb        = fb;
    s_fb_w      = fb_w;
    s_fb_h      = fb_h;
    s_fireplace = fireplace_img;
    s_fire_sz   = fireplace_sz;
    s_mutex     = xSemaphoreCreateMutex();

    esp_timer_handle_t timer;
    const esp_timer_create_args_t args = {
        .callback = poll_timer_cb,
        .name     = "pm_poll",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &timer), TAG, "timer create failed");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(timer, POLL_PERIOD_US), TAG, "timer start failed");

    esp_timer_handle_t status_timer;
    const esp_timer_create_args_t status_args = {
        .callback = status_poll_cb,
        .name     = "status_poll",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&status_args, &status_timer), TAG, "status timer create failed");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(status_timer, STATUS_POLL_PERIOD_US), TAG, "status timer start failed");

    ESP_LOGI(TAG, "Started — time poll every 60 s, status poll every 5 s (when backlight on)");
    return ESP_OK;
}

void power_manager_on_time_synced(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool was_error = s_error_shown;
    s_sync_pending = false;
    s_error_shown  = false;
    xSemaphoreGive(s_mutex);

    if (was_error) {
        show_fireplace();
        gui_redraw_status();
    }
}

bool power_manager_on_touch(void)
{
    int64_t now = esp_timer_get_time();

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool is_awake = (now < s_awake_until_us);
    if (is_awake) {
        /* Extend the awake window — stays in sync with backlight override */
        s_awake_until_us = now + AWAKE_PERIOD_US;
    }
    xSemaphoreGive(s_mutex);

    if (is_awake) {
        return false;   /* pass through to GUI */
    }

    /* Display is dim — consume this touch and wake the backlight */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_awake_until_us = now + AWAKE_PERIOD_US;
    xSemaphoreGive(s_mutex);

    backlight_manager_on_button_press();
    ESP_LOGI(TAG, "Wake touch — backlight activated, event swallowed");
    return true;    /* do NOT pass to gui_handle_touch */
}
