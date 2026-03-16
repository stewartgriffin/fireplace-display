#include "logic.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_check.h"
#include "comm.h"
#include "gui.h"
#include "backlight_manager.h"

static const char *TAG = "logic";

/* 60-second time-sync poll period */
#define POLL_PERIOD_US          (60LL * 1000000LL)
/* 5-second status poll period */
#define STATUS_POLL_PERIOD_US   (5LL * 1000000LL)
/* 1-second dTdt poll period */
#define DTDT_POLL_PERIOD_US     (1LL * 1000000LL)

static SemaphoreHandle_t  mutex;
static esp_timer_handle_t status_timer = NULL;
static bool               sync_pending = false;
static bool               error_shown  = false;

/* Session dTdt maxes — reset when combustion_state transitions to 0 */
static int16_t  session_max[3];          /* [0]=10s [1]=20s [2]=30s */
static uint8_t  prev_combustion_state = 0xFF; /* sentinel: unset */

static const uint8_t *fireplace_data = NULL;
static size_t         fireplace_size = 0;

/* ------------------------------------------------------------------ */
/*  Timer callbacks                                                     */
/* ------------------------------------------------------------------ */
static void poll_timer_cb(void *arg)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool was_pending  = sync_pending;
    bool error_before = error_shown;
    sync_pending = true;
    xSemaphoreGive(mutex);

    if (was_pending && !error_before) {
        xSemaphoreTake(mutex, portMAX_DELAY);
        error_shown = true;
        xSemaphoreGive(mutex);
        gui_show_error("Komunikacja ze", "sterownikiem", "przerwana");
    }

    comm_request_time();
    ESP_LOGD(TAG, "Time poll sent");
}

static void dTdt_poll_cb(void *arg)
{
    comm_request_dTdt();
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
esp_err_t logic_init(const uint8_t *fireplace_img, size_t fireplace_sz)
{
    fireplace_data = fireplace_img;
    fireplace_size = fireplace_sz;
    mutex          = xSemaphoreCreateMutex();

    /* comm_init already sent the initial time request (with retry).
     * Mark sync as pending so poll_timer_cb can detect a missed response. */
    sync_pending = true;

    esp_timer_handle_t timer;
    const esp_timer_create_args_t poll_args = {
        .callback = poll_timer_cb,
        .name     = "logic_poll",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&poll_args, &timer), TAG, "poll timer create failed");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(timer, POLL_PERIOD_US), TAG, "poll timer start failed");

    const esp_timer_create_args_t status_args = {
        .callback = status_poll_cb,
        .name     = "logic_status",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&status_args, &status_timer), TAG, "status timer create failed");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(status_timer, STATUS_POLL_PERIOD_US), TAG, "status timer start failed");

    esp_timer_handle_t dTdt_timer;
    const esp_timer_create_args_t dTdt_args = {
        .callback = dTdt_poll_cb,
        .name     = "logic_dTdt",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&dTdt_args, &dTdt_timer), TAG, "dTdt timer create failed");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(dTdt_timer, DTDT_POLL_PERIOD_US), TAG, "dTdt timer start failed");

    ESP_LOGI(TAG, "Started — time poll every 60 s, status poll every 5 s, dTdt poll every 1 s");
    return ESP_OK;
}

void logic_on_time_synced(void)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool was_error = error_shown;
    sync_pending   = false;
    error_shown    = false;
    xSemaphoreGive(mutex);

    if (was_error) {
        gui_show_fireplace(fireplace_data, fireplace_size);
        gui_redraw_status();
    }
}

void logic_on_status(const combustion_controler_info_t *info)
{
    if (prev_combustion_state != 0xFF && prev_combustion_state != 0 && info->combustion_state == 0) {
        session_max[0] = 0;
        session_max[1] = 0;
        session_max[2] = 0;
        ESP_LOGI(TAG, "Combustion off — session dTdt maxes reset");
    }
    prev_combustion_state = info->combustion_state;
    gui_on_controller_info(info);
}

void logic_on_dTdt(const dTdt_data_t *data)
{
    if (data->sliding_max_10s > session_max[0]) session_max[0] = data->sliding_max_10s;
    if (data->sliding_max_20s > session_max[1]) session_max[1] = data->sliding_max_20s;
    if (data->sliding_max_30s > session_max[2]) session_max[2] = data->sliding_max_30s;
    gui_on_dTdt(data, session_max);
}

bool logic_on_touch(void)
{
    /* In evening mode (display dimmed by schedule) touches pass straight through.
     * Only consume the first tap when the display is completely off (standby mode). */
    if (backlight_manager_is_on()) {
        return false;   /* evening mode or override active — pass through to GUI */
    }

    /* Display is completely off — consume this touch and wake the backlight */
    backlight_manager_on_button_press();

    /* Request status immediately and reset the periodic timer so the next
     * periodic fire is exactly STATUS_POLL_PERIOD_US from now — no double-send. */
    comm_request_status();
    if (status_timer) {
        esp_timer_restart(status_timer, STATUS_POLL_PERIOD_US);
    }

    ESP_LOGI(TAG, "Wake touch — backlight activated from standby, event swallowed");
    return true;
}
