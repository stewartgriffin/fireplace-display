#include "comm.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_check.h"

static const char *TAG = "comm";

/* ---------- UART configuration ---------- */
#define COMM_UART_NUM       UART_NUM_1
#define COMM_TX_PIN         GPIO_NUM_47
#define COMM_RX_PIN         GPIO_NUM_48
#define COMM_BAUD_RATE      115200

/* ---------- Protocol constants ---------- */
#define COMM_SOF            0xAA
#define COMM_MAX_PAYLOAD    8
#define COMM_ACK_TIMEOUT_MS 300
#define COMM_TIME_POLL_US   (60LL * 1000 * 1000)

/* ---------- Message IDs ---------- */
#define MSG_FIREPLACE_ENABLE    0x01
#define MSG_FIREPLACE_DISABLE   0x02
#define MSG_VENTILATION_ENABLE  0x03
#define MSG_VENTILATION_DISABLE 0x04
#define MSG_TIME_REQUEST        0x05
#define MSG_ACK                 0x81
#define MSG_NACK                0x82
#define MSG_TIME_RESPONSE       0x83

/* ---------- Internal state ---------- */
typedef struct {
    uint8_t type;       /* MSG_ACK or MSG_NACK */
    uint8_t echoed_id;  /* MSG_ID from payload[0] */
} ack_result_t;

static comm_time_cb_t  s_time_cb;
static QueueHandle_t   s_ack_queue;
static SemaphoreHandle_t s_tx_mutex;

/* ---------- CRC16-Modbus ---------- */
static uint16_t crc16_modbus(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc & 0x0001) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
        }
    }
    return crc;
}

/* ---------- Frame send ---------- */
static esp_err_t send_frame(uint8_t msg_id, const uint8_t *payload, uint8_t len)
{
    uint8_t frame[5 + COMM_MAX_PAYLOAD];
    frame[0] = COMM_SOF;
    frame[1] = msg_id;
    frame[2] = len;
    if (len > 0 && payload) {
        memcpy(&frame[3], payload, len);
    }
    uint16_t crc = crc16_modbus(&frame[1], 2 + len);  /* covers msg_id + len + payload */
    frame[3 + len] = (crc >> 8) & 0xFF;
    frame[4 + len] =  crc       & 0xFF;

    int written = uart_write_bytes(COMM_UART_NUM, frame, 5 + len);
    return (written == 5 + len) ? ESP_OK : ESP_FAIL;
}

/* ---------- Command send + ACK wait ---------- */
static esp_err_t send_command(uint8_t msg_id)
{
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);

    /* Drain stale ACKs from previous timed-out commands */
    ack_result_t stale;
    while (xQueueReceive(s_ack_queue, &stale, 0) == pdTRUE) {}

    esp_err_t ret = send_frame(msg_id, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Send failed for msg 0x%02x", msg_id);
        xSemaphoreGive(s_tx_mutex);
        return ret;
    }

    ack_result_t result;
    if (xQueueReceive(s_ack_queue, &result, pdMS_TO_TICKS(COMM_ACK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "ACK timeout for msg 0x%02x", msg_id);
        xSemaphoreGive(s_tx_mutex);
        return ESP_ERR_TIMEOUT;
    }

    xSemaphoreGive(s_tx_mutex);

    if (result.type == MSG_NACK) {
        ESP_LOGW(TAG, "NACK received for msg 0x%02x", msg_id);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ---------- RX task ---------- */
static void rx_task(void *arg)
{
    while (1) {
        uint8_t sof;
        if (uart_read_bytes(COMM_UART_NUM, &sof, 1, pdMS_TO_TICKS(100)) != 1 || sof != COMM_SOF) {
            continue;
        }

        uint8_t header[2];
        if (uart_read_bytes(COMM_UART_NUM, header, 2, pdMS_TO_TICKS(50)) != 2) continue;
        uint8_t msg_id = header[0];
        uint8_t len    = header[1];

        if (len > COMM_MAX_PAYLOAD) {
            ESP_LOGW(TAG, "Oversized payload (%d), discarding", len);
            continue;
        }

        uint8_t payload[COMM_MAX_PAYLOAD] = {0};
        if (len > 0 && uart_read_bytes(COMM_UART_NUM, payload, len, pdMS_TO_TICKS(50)) != len) continue;

        uint8_t crc_bytes[2];
        if (uart_read_bytes(COMM_UART_NUM, crc_bytes, 2, pdMS_TO_TICKS(50)) != 2) continue;

        uint16_t received_crc = ((uint16_t)crc_bytes[0] << 8) | crc_bytes[1];
        uint8_t  crc_buf[2 + COMM_MAX_PAYLOAD] = {msg_id, len};
        memcpy(&crc_buf[2], payload, len);
        uint16_t computed_crc = crc16_modbus(crc_buf, 2 + len);

        if (received_crc != computed_crc) {
            ESP_LOGW(TAG, "CRC error on msg 0x%02x (got 0x%04x, expected 0x%04x)",
                     msg_id, received_crc, computed_crc);
            continue;
        }

        switch (msg_id) {
            case MSG_ACK:
            case MSG_NACK: {
                ack_result_t r = {
                    .type      = msg_id,
                    .echoed_id = len >= 1 ? payload[0] : 0,
                };
                xQueueSend(s_ack_queue, &r, 0);
                break;
            }
            case MSG_TIME_RESPONSE: {
                if (len < 7) {
                    ESP_LOGW(TAG, "Short TIME_RESPONSE (%d bytes)", len);
                    break;
                }
                struct tm t = {
                    .tm_year = (((int)payload[0] << 8) | payload[1]) - 1900,
                    .tm_mon  = payload[2] - 1,
                    .tm_mday = payload[3],
                    .tm_hour = payload[4],
                    .tm_min  = payload[5],
                    .tm_sec  = payload[6],
                    .tm_isdst = -1,
                };
                if (s_time_cb) s_time_cb(&t);
                break;
            }
            default:
                ESP_LOGW(TAG, "Unknown msg_id 0x%02x", msg_id);
                break;
        }
    }
}

/* ---------- Periodic time poll ---------- */
static void time_poll_cb(void *arg)
{
    comm_request_time();
}

/* ---------- Public API ---------- */

esp_err_t comm_init(comm_time_cb_t on_time_received)
{
    s_time_cb  = on_time_received;
    s_ack_queue = xQueueCreate(1, sizeof(ack_result_t));
    s_tx_mutex  = xSemaphoreCreateMutex();

    const uart_config_t uart_cfg = {
        .baud_rate  = COMM_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_param_config(COMM_UART_NUM, &uart_cfg), TAG, "UART param config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(COMM_UART_NUM, COMM_TX_PIN, COMM_RX_PIN,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "UART set pin failed");
    ESP_RETURN_ON_ERROR(uart_driver_install(COMM_UART_NUM, 2048, 2048, 0, NULL, 0), TAG, "UART driver install failed");
    ESP_RETURN_ON_ERROR(uart_set_mode(COMM_UART_NUM, UART_MODE_RS485_HALF_DUPLEX), TAG, "UART RS485 mode failed");

    xTaskCreate(rx_task, "comm_rx", 4096, NULL, 5, NULL);

    const esp_timer_create_args_t timer_args = {
        .callback = time_poll_cb,
        .name     = "comm_time_poll",
    };
    esp_timer_handle_t timer;
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &timer), TAG, "Timer create failed");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(timer, COMM_TIME_POLL_US), TAG, "Timer start failed");

    ESP_LOGI(TAG, "Initialised at %d baud", COMM_BAUD_RATE);
    comm_request_time();  /* sync time on startup */
    return ESP_OK;
}

esp_err_t comm_fireplace_enable(void)    { return send_command(MSG_FIREPLACE_ENABLE);    }
esp_err_t comm_fireplace_disable(void)   { return send_command(MSG_FIREPLACE_DISABLE);   }
esp_err_t comm_ventilation_enable(void)  { return send_command(MSG_VENTILATION_ENABLE);  }
esp_err_t comm_ventilation_disable(void) { return send_command(MSG_VENTILATION_DISABLE); }

esp_err_t comm_request_time(void)
{
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    esp_err_t ret = send_frame(MSG_TIME_REQUEST, NULL, 0);
    xSemaphoreGive(s_tx_mutex);
    return ret;
}
