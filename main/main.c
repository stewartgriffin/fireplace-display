#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_cache.h"
#include "comm.h"

static const char *TAG = "fireplace";

/* ---------- Pin definitions ---------- */
#define LCD_BACKLIGHT_GPIO      GPIO_NUM_26
#define LCD_RESET_GPIO          GPIO_NUM_27

/* ---------- Display parameters (ST7703, 720×720) ---------- */
#define LCD_H_RES               720
#define LCD_V_RES               720
#define LCD_MIPI_DSI_LANES      2
#define LCD_MIPI_DSI_MBPS       480
#define LCD_DPI_CLK_MHZ         38
#define LCD_NUM_FB              1

/* ---------- MIPI DSI PHY power (on-chip LDO) ---------- */
#define MIPI_DSI_LDO_CHAN       3
#define MIPI_DSI_LDO_MV         2500

/* ---------- Backlight (LEDC) ---------- */
#define LCD_BL_LEDC_TIMER       LEDC_TIMER_1
#define LCD_BL_LEDC_CH          LEDC_CHANNEL_1
#define LCD_BL_LEDC_DUTY_RES    LEDC_TIMER_10_BIT
#define LCD_BL_LEDC_FREQ_HZ     5000

static esp_lcd_panel_handle_t s_panel = NULL;

static void on_time_received(const struct tm *t)
{
    struct timeval tv = { .tv_sec = mktime((struct tm *)t), .tv_usec = 0 };
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "Time set: %04d-%02d-%02d %02d:%02d:%02d",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);
}

static esp_err_t display_backlight_init(void)
{
    const ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LCD_BL_LEDC_DUTY_RES,
        .timer_num       = LCD_BL_LEDC_TIMER,
        .freq_hz         = LCD_BL_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "LEDC timer config failed");

    const ledc_channel_config_t ch_cfg = {
        .gpio_num   = LCD_BACKLIGHT_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LCD_BL_LEDC_CH,
        .timer_sel  = LCD_BL_LEDC_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .duty       = 0,
        .hpoint     = 0,
        .flags      = {.output_invert = 1},
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch_cfg), TAG, "LEDC channel config failed");

    return ESP_OK;
}

static esp_err_t display_set_brightness(int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;

    uint32_t duty = ((1 << LCD_BL_LEDC_DUTY_RES) - 1) * percent / 100;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_BL_LEDC_CH, duty),
                        TAG, "LEDC set duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_BL_LEDC_CH),
                        TAG, "LEDC update duty failed");
    return ESP_OK;
}

/* Send a single command + optional data bytes over DBI */
#define LCD_CMD(io, cmd, ...) do { \
    uint8_t _d[] = {__VA_ARGS__}; \
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param((io), (cmd), \
        sizeof(_d) ? _d : NULL, sizeof(_d)), TAG, "LCD cmd 0x%02x failed", (cmd)); \
} while(0)

static esp_err_t display_init(void)
{
    /* Power on the MIPI DSI PHY via the on-chip LDO */
    static esp_ldo_channel_handle_t s_phy_ldo = NULL;
    const esp_ldo_channel_config_t ldo_cfg = {
        .chan_id    = MIPI_DSI_LDO_CHAN,
        .voltage_mv = MIPI_DSI_LDO_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &s_phy_ldo),
                        TAG, "LDO acquire failed");
    ESP_LOGI(TAG, "MIPI DSI PHY powered on");

    /* Create MIPI DSI bus */
    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    const esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id             = 0,
        .num_data_lanes     = LCD_MIPI_DSI_LANES,
        .phy_clk_src        = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = LCD_MIPI_DSI_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus),
                        TAG, "DSI bus init failed");

    /* Create DBI (command) panel IO */
    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits    = 8,
        .lcd_param_bits  = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &io),
                        TAG, "Panel IO init failed");

    /* Hardware reset */
    gpio_config_t rst_cfg = {
        .pin_bit_mask = BIT64(LCD_RESET_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rst_cfg);
    gpio_set_level(LCD_RESET_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(LCD_RESET_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    /* ST7703 initialisation sequence (Waveshare ESP32-P4-86-Panel) */
    LCD_CMD(io, 0xB9, 0xF1, 0x12, 0x83);
    LCD_CMD(io, 0xB1, 0x00, 0x00, 0x00, 0xDA, 0x80);
    LCD_CMD(io, 0xB2, 0x3C, 0x12, 0x30);
    LCD_CMD(io, 0xB3, 0x10, 0x10, 0x28, 0x28, 0x03, 0xFF, 0x00, 0x00, 0x00, 0x00);
    LCD_CMD(io, 0xB4, 0x80);
    LCD_CMD(io, 0xB5, 0x0A, 0x0A);
    LCD_CMD(io, 0xB6, 0x97, 0x97);
    LCD_CMD(io, 0xB8, 0x26, 0x22, 0xF0, 0x13);
    LCD_CMD(io, 0xBA, 0x31, 0x81, 0x0F, 0xF9, 0x0E, 0x06, 0x20, 0x00,
                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x44, 0x25,
                      0x00, 0x90, 0x0A, 0x00, 0x00, 0x01, 0x4F, 0x01,
                      0x00, 0x00, 0x37);
    LCD_CMD(io, 0xBC, 0x47);
    LCD_CMD(io, 0xBF, 0x02, 0x11, 0x00);
    LCD_CMD(io, 0xC0, 0x73, 0x73, 0x50, 0x50, 0x00, 0x00, 0x12, 0x70, 0x00);
    LCD_CMD(io, 0xC1, 0x25, 0x00, 0x32, 0x32, 0x77, 0xE4, 0xFF, 0xFF,
                      0xCC, 0xCC, 0x77, 0x77);
    LCD_CMD(io, 0xC6, 0x82, 0x00, 0xBF, 0xFF, 0x00, 0xFF);
    LCD_CMD(io, 0xC7, 0xB8, 0x00, 0x0A, 0x10, 0x01, 0x09);
    LCD_CMD(io, 0xC8, 0x10, 0x40, 0x1E, 0x02);
    LCD_CMD(io, 0xCC, 0x0B);
    LCD_CMD(io, 0xE0, 0x00, 0x0B, 0x10, 0x2C, 0x3D, 0x3F, 0x42, 0x3A,
                      0x07, 0x0D, 0x0F, 0x13, 0x15, 0x13, 0x14, 0x0F,
                      0x16, 0x00, 0x0B, 0x10, 0x2C, 0x3D, 0x3F, 0x42,
                      0x3A, 0x07, 0x0D, 0x0F, 0x13, 0x15, 0x13, 0x14,
                      0x0F, 0x16);
    LCD_CMD(io, 0xE3, 0x07, 0x07, 0x0B, 0x0B, 0x0B, 0x0B, 0x00, 0x00,
                      0x00, 0x00, 0xFF, 0x00, 0xC0, 0x10);
    LCD_CMD(io, 0xE9, 0xC8, 0x10, 0x0A, 0x00, 0x00, 0x80, 0x81, 0x12,
                      0x31, 0x23, 0x4F, 0x86, 0xA0, 0x00, 0x47, 0x08,
                      0x00, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00,
                      0x0C, 0x00, 0x00, 0x00, 0x98, 0x02, 0x8B, 0xAF,
                      0x46, 0x02, 0x88, 0x88, 0x88, 0x88, 0x88, 0x98,
                      0x13, 0x8B, 0xAF, 0x57, 0x13, 0x88, 0x88, 0x88,
                      0x88, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
    LCD_CMD(io, 0xEA, 0x97, 0x0C, 0x09, 0x09, 0x09, 0x78, 0x00, 0x00,
                      0x00, 0x00, 0x00, 0x00, 0x9F, 0x31, 0x8B, 0xA8,
                      0x31, 0x75, 0x88, 0x88, 0x88, 0x88, 0x88, 0x9F,
                      0x20, 0x8B, 0xA8, 0x20, 0x64, 0x88, 0x88, 0x88,
                      0x88, 0x88, 0x23, 0x00, 0x00, 0x02, 0x71, 0x00,
                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x80,
                      0x81, 0x00, 0x00, 0x00, 0x00);
    LCD_CMD(io, 0xEF, 0xFF, 0xFF, 0x01);

    /* Sleep out then display on */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x11, NULL, 0),
                        TAG, "Sleep out failed");
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x29, NULL, 0),
                        TAG, "Display on failed");

    /* Create DPI (pixel stream) panel */
    const esp_lcd_dpi_panel_config_t dpi_cfg = {
        .dpi_clk_src        = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = LCD_DPI_CLK_MHZ,
        .virtual_channel    = 0,
        .pixel_format       = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs            = LCD_NUM_FB,
        .video_timing = {
            .h_size           = LCD_H_RES,
            .v_size           = LCD_V_RES,
            .hsync_back_porch = 50,
            .hsync_pulse_width= 20,
            .hsync_front_porch= 50,
            .vsync_back_porch = 20,
            .vsync_pulse_width= 4,
            .vsync_front_porch= 20,
        },
        .flags.use_dma2d = true,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_dpi(dsi_bus, &dpi_cfg, &s_panel),
                        TAG, "DPI panel create failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "Panel init failed");

    ESP_LOGI(TAG, "Display initialised (%dx%d)", LCD_H_RES, LCD_V_RES);
    return ESP_OK;
}

/* Embedded fireplace image (720×720 RGB565, little-endian) */
extern const uint8_t fireplace_bin_start[] asm("_binary_fireplace_bin_start");
extern const uint8_t fireplace_bin_end[]   asm("_binary_fireplace_bin_end");

void app_main(void)
{
    ESP_ERROR_CHECK(comm_init(on_time_received));
    ESP_ERROR_CHECK(display_backlight_init());
    ESP_ERROR_CHECK(display_init());

    /* Copy fireplace image into the DPI frame buffer */
    void *fb = NULL;
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &fb));
    size_t fb_size = LCD_H_RES * LCD_V_RES * sizeof(uint16_t);
    memcpy(fb, fireplace_bin_start, fb_size);
    esp_cache_msync(fb, fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    /* Turn on backlight at full brightness */
    ESP_ERROR_CHECK(display_set_brightness(100));

    ESP_LOGI(TAG, "Display ready");
}
