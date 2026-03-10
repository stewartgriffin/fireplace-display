#include "touch.h"
#include "driver/i2c_master.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch";

/* ---------- Pin assignments (Waveshare ESP32-P4-86-Panel) ---------- */
#define TOUCH_I2C_SDA       GPIO_NUM_7
#define TOUCH_I2C_SCL       GPIO_NUM_8
#define TOUCH_RST_GPIO      GPIO_NUM_23
#define TOUCH_INT_GPIO      GPIO_NUM_NC   /* not connected on this board */
#define TOUCH_I2C_FREQ_HZ   400000
#define TOUCH_X_MAX         720
#define TOUCH_Y_MAX         720

static esp_lcd_touch_handle_t s_touch = NULL;
static touch_event_cb_t s_cb = NULL;

static void touch_task(void *arg)
{
    esp_lcd_touch_point_data_t points[1];
    uint8_t cnt = 0;
    bool was_touched = false;

    while (1) {
        esp_lcd_touch_read_data(s_touch);
        esp_lcd_touch_get_data(s_touch, points, &cnt, 1);
        bool touching = (cnt > 0);
        if (touching && !was_touched && s_cb) {
            s_cb(points[0].x, points[0].y);
        }
        was_touched = touching;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t touch_init(touch_event_cb_t on_touch)
{
    s_cb = on_touch;

    /* Initialize I2C master bus */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = TOUCH_I2C_SDA,
        .scl_io_num = TOUCH_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,
    };
    i2c_master_bus_handle_t bus;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "I2C init failed");

    /* Create panel IO for GT911 over I2C */
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io_cfg.scl_speed_hz = TOUCH_I2C_FREQ_HZ;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_i2c(bus, &tp_io_cfg, &tp_io),
        TAG, "GT911 panel IO init failed");

    /* Initialize GT911 */
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = TOUCH_X_MAX,
        .y_max = TOUCH_Y_MAX,
        .rst_gpio_num = TOUCH_RST_GPIO,
        .int_gpio_num = TOUCH_INT_GPIO,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &s_touch),
        TAG, "GT911 init failed");

    xTaskCreate(touch_task, "touch", 2048, NULL, 5, NULL);
    ESP_LOGI(TAG, "Touch initialized (GT911, SDA=%d, SCL=%d, RST=%d)",
             TOUCH_I2C_SDA, TOUCH_I2C_SCL, TOUCH_RST_GPIO);
    return ESP_OK;
}
