#include "power_manager.h"

#include "esp_pm.h"
#include "esp_log.h"

static const char *TAG = "power_mgr";

static esp_pm_lock_handle_t cpu_lock  = NULL;
static bool                 lock_held = false;

esp_err_t power_manager_init(void)
{
    /* Configure dynamic frequency scaling.
     * Light sleep is intentionally disabled: the MIPI DSI controller requires
     * a continuous pixel clock — entering light sleep would halt it. */
    const esp_pm_config_t pm_cfg = {
        .max_freq_mhz       = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz       = 40,
        .light_sleep_enable = false,
    };
    esp_err_t ret = esp_pm_configure(&pm_cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_configure: %s — frequency scaling inactive", esp_err_to_name(ret));
    }

    ret = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "display_active", &cpu_lock);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "PM lock create: %s — lock unavailable", esp_err_to_name(ret));
        cpu_lock = NULL;
        return ESP_OK;  /* non-fatal: system runs without PM */
    }

    /* Acquire at boot — display is active immediately */
    esp_pm_lock_acquire(cpu_lock);
    lock_held = true;

    ESP_LOGI(TAG, "Initialised (light sleep disabled — MIPI DSI constraint)");
    return ESP_OK;
}

void power_manager_set_active(bool active)
{
    if (!cpu_lock) return;

    if (active && !lock_held) {
        esp_pm_lock_acquire(cpu_lock);
        lock_held = true;
        ESP_LOGI(TAG, "CPU freq lock acquired — display active");
    } else if (!active && lock_held) {
        esp_pm_lock_release(cpu_lock);
        lock_held = false;
        ESP_LOGI(TAG, "CPU freq lock released — display idle");
    }
}
