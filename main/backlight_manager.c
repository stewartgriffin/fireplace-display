#include "backlight_manager.h"

#include <time.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "backlight";

#define DAY_BRIGHTNESS_DEFAULT  20
#define DAY_BRIGHTNESS_MIN      10
#define DAY_BRIGHTNESS_MAX      100
#define DAY_BRIGHTNESS_STEP     10
#define BUTTON_OVERRIDE_MS      (60 * 1000LL)
#define TASK_PERIOD_MS          60000   /* re-evaluate every minute */

/* Backlight turns off after this hour every day */
#define NIGHT_START_HOUR    23

/* Evening turn-on time varies by day of year (cosine interpolation, 1-minute resolution).
 * Reference points:  Dec 24 (tm_yday=357) → 15:00 (900 min)
 *                    Jun 20 (tm_yday=170) → 20:45 (1245 min)
 * Solved: center=1072.75, amplitude=172.75  */
#define SEASONAL_CENTER     1072.75
#define SEASONAL_AMPLITUDE   172.75
#define DEC24_YDAY           357    /* 0-based tm_yday for Dec 24 (non-leap year) */

static int seasonal_on_minutes(int yday)  /* returns minutes since midnight */
{
    double m = SEASONAL_CENTER
               - SEASONAL_AMPLITUDE * cos(2.0 * M_PI * (yday - DEC24_YDAY) / 365.0);
    return (int)round(m);
}

static backlight_set_fn  s_set_fn;
static int               s_day_brightness    = DAY_BRIGHTNESS_DEFAULT;
static int               s_current_brightness = 0;
static int64_t           s_button_press_us   = 0;  /* 0 = no active override */
static SemaphoreHandle_t s_mutex;

/* Compute and apply the current brightness level. May be called from any task. */
static void apply_backlight(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int brightness;
    bool use_override = false;

    if (s_button_press_us > 0) {
        int64_t elapsed_ms = (esp_timer_get_time() - s_button_press_us) / 1000;
        if (elapsed_ms < BUTTON_OVERRIDE_MS) {
            use_override = true;
        } else {
            s_button_press_us = 0;
            ESP_LOGI(TAG, "Button override expired, resuming schedule");
        }
    }

    if (use_override) {
        brightness = 100;
    } else {
        time_t now;
        time(&now);
        struct tm t;
        localtime_r(&now, &t);
        int current_min = t.tm_hour * 60 + t.tm_min;
        int on_min      = seasonal_on_minutes(t.tm_yday);
        int night_min   = NIGHT_START_HOUR * 60;
        bool on = (current_min >= on_min && current_min < night_min);
        brightness = on ? s_day_brightness : 0;
    }

    s_current_brightness = brightness;
    xSemaphoreGive(s_mutex);
    s_set_fn(brightness);
}

bool backlight_manager_is_on(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool on = s_current_brightness > 0;
    xSemaphoreGive(s_mutex);
    return on;
}

static void backlight_task(void *arg)
{
    while (1) {
        apply_backlight();
        vTaskDelay(pdMS_TO_TICKS(TASK_PERIOD_MS));
    }
}

esp_err_t backlight_manager_init(backlight_set_fn set_brightness)
{
    s_set_fn = set_brightness;
    s_mutex  = xSemaphoreCreateMutex();
    xTaskCreate(backlight_task, "backlight", 2048, NULL, 3, NULL);
    ESP_LOGI(TAG, "Started (day=%d%%, step=%d%%, on-time seasonal Dec24=15:00 Jun20=20:45)",
             DAY_BRIGHTNESS_DEFAULT, DAY_BRIGHTNESS_STEP);
    return ESP_OK;
}

void backlight_manager_up(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_day_brightness += DAY_BRIGHTNESS_STEP;
    if (s_day_brightness > DAY_BRIGHTNESS_MAX) s_day_brightness = DAY_BRIGHTNESS_MAX;
    int b = s_day_brightness;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Brightness up → %d%%", b);
    apply_backlight();
}

void backlight_manager_down(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_day_brightness -= DAY_BRIGHTNESS_STEP;
    if (s_day_brightness < DAY_BRIGHTNESS_MIN) s_day_brightness = DAY_BRIGHTNESS_MIN;
    int b = s_day_brightness;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Brightness down → %d%%", b);
    apply_backlight();
}

void backlight_manager_on_button_press(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_button_press_us = esp_timer_get_time();
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Button press: 100%% for 60 s");
    apply_backlight();
}
