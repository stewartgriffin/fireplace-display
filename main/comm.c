#include "comm.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"

static const char *TAG = "comm";

/* ---------- UART configuration ---------- */
#define COMM_UART_NUM       UART_NUM_1
#define COMM_TX_PIN         GPIO_NUM_47
#define COMM_RX_PIN         GPIO_NUM_48
#define COMM_BAUD_RATE      115200

/* ---------- Protocol constants ---------- */
#define COMM_SOF            0xAA
#define COMM_MAX_PAYLOAD    12
#define COMM_ACK_TIMEOUT_MS 300

/* Maximum time to wait for a response after sending a request.
   Covers the round-trip on a 115200-baud RS485 link plus STM32 processing. */
#define COMM_RESP_TIMEOUT_MS 400

/* ---------- Message IDs ---------- */
#define MSG_FIREPLACE_ENABLE    0x01
#define MSG_FIREPLACE_DISABLE   0x02
#define MSG_VENTILATION_ENABLE  0x03
#define MSG_VENTILATION_DISABLE 0x04
#define MSG_TIME_REQUEST        0x05
#define MSG_ACK                 0x81
#define MSG_NACK                0x82
#define MSG_TIME_RESPONSE       0x83
#define MSG_STATUS_REQUEST      0x06
#define MSG_STATUS_RESPONSE     0x84
#define MSG_DTDT_REQUEST        0x07
#define MSG_DTDT_RESPONSE       0x85

/* ---------- Time-sync retry ---------- */
#define TIME_RETRY_US   (1LL * 1000000LL)
#define TIME_RETRY_MAX  5

/* ---------- Internal state ---------- */
typedef struct {
    uint8_t type;       /* MSG_ACK or MSG_NACK */
    uint8_t echoed_id;  /* MSG_ID from payload[0] */
} ack_result_t;

static comm_time_cb_t    time_cb;
static comm_status_cb_t  status_cb;
static comm_dTdt_cb_t    dTdt_cb;
static QueueHandle_t     ack_queue;
static SemaphoreHandle_t tx_mutex;
static esp_timer_handle_t time_retry_timer = NULL;
static volatile int       time_retry_count = 0;
static volatile bool      time_sync_pending = false;

/* ---------- Request queue ---------- */
#define COMM_REQ_QUEUE_DEPTH 8

static QueueHandle_t     req_queue;  /* holds uint8_t msg_id */
static SemaphoreHandle_t resp_sem;   /* given by rx_task on each valid response */

/* Dedup bits — set when enqueued, cleared when dequeued by tx_task.
   Prevents the same request type from piling up in the queue. */
#define PEND_TIME   0x01
#define PEND_STATUS 0x02
#define PEND_DTDT   0x04
static volatile uint8_t pending_bits = 0;
static portMUX_TYPE     pend_mux = portMUX_INITIALIZER_UNLOCKED;

static uint8_t msg_to_pend_bit(uint8_t msg_id)
{
    switch (msg_id) {
        case MSG_TIME_REQUEST:   return PEND_TIME;
        case MSG_STATUS_REQUEST: return PEND_STATUS;
        case MSG_DTDT_REQUEST:   return PEND_DTDT;
        default:                 return 0;
    }
}

/* Enqueue a request for tx_task. Returns false if the queue is full or the
   same request type is already queued (dedup). */
static bool enqueue_req(uint8_t msg_id)
{
    uint8_t bit = msg_to_pend_bit(msg_id);
    if (bit) {
        taskENTER_CRITICAL(&pend_mux);
        bool already = (pending_bits & bit) != 0;
        if (!already) pending_bits |= bit;
        taskEXIT_CRITICAL(&pend_mux);
        if (already) return true;  /* already queued — skip silently */
    }
    if (xQueueSend(req_queue, &msg_id, 0) != pdTRUE) {
        if (bit) {
            taskENTER_CRITICAL(&pend_mux);
            pending_bits &= ~bit;
            taskEXIT_CRITICAL(&pend_mux);
        }
        ESP_LOGW(TAG, "req_queue full, dropping 0x%02x", msg_id);
        return false;
    }
    return true;
}

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
    xSemaphoreTake(tx_mutex, portMAX_DELAY);

    /* Drain stale ACKs from previous timed-out commands */
    ack_result_t stale;
    while (xQueueReceive(ack_queue, &stale, 0) == pdTRUE) {}

    esp_err_t ret = send_frame(msg_id, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Send failed for msg 0x%02x", msg_id);
        xSemaphoreGive(tx_mutex);
        return ret;
    }

    ack_result_t result;
    if (xQueueReceive(ack_queue, &result, pdMS_TO_TICKS(COMM_ACK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "ACK timeout for msg 0x%02x", msg_id);
        xSemaphoreGive(tx_mutex);
        return ESP_ERR_TIMEOUT;
    }

    xSemaphoreGive(tx_mutex);

    if (result.type == MSG_NACK) {
        ESP_LOGW(TAG, "NACK received for msg 0x%02x", msg_id);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ---------- TX task — serialises all poll requests ---------- */
static void tx_task(void *arg)
{
    uint8_t msg_id;
    while (1) {
        xQueueReceive(req_queue, &msg_id, portMAX_DELAY);

        /* Clear dedup bit so the same request type can be enqueued again */
        uint8_t bit = msg_to_pend_bit(msg_id);
        if (bit) {
            taskENTER_CRITICAL(&pend_mux);
            pending_bits &= ~bit;
            taskEXIT_CRITICAL(&pend_mux);
        }

        xSemaphoreTake(tx_mutex, portMAX_DELAY);
        xSemaphoreTake(resp_sem, 0);   /* flush any stale signal */
        send_frame(msg_id, NULL, 0);
        bool got = (xSemaphoreTake(resp_sem, pdMS_TO_TICKS(COMM_RESP_TIMEOUT_MS)) == pdTRUE);
        xSemaphoreGive(tx_mutex);

        if (!got) {
            ESP_LOGD(TAG, "No response for req 0x%02x", msg_id);
        }
    }
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
                xQueueSend(ack_queue, &r, 0);
                break;
            }
            case MSG_TIME_RESPONSE: {
                if (len < 7) {
                    ESP_LOGW(TAG, "Short TIME_RESPONSE (%d bytes)", len);
                    break;
                }
                time_sync_pending = false;
                esp_timer_stop(time_retry_timer);
                struct tm t = {
                    .tm_year = (((int)payload[0] << 8) | payload[1]) - 1900,
                    .tm_mon  = payload[2] - 1,
                    .tm_mday = payload[3],
                    .tm_hour = payload[4],
                    .tm_min  = payload[5],
                    .tm_sec  = payload[6],
                    .tm_isdst = -1,
                };
                if (time_cb) time_cb(&t);
                xSemaphoreGive(resp_sem);
                break;
            }
            case MSG_STATUS_RESPONSE: {
                if (len < 7) {
                    ESP_LOGW(TAG, "Short STATUS_RESPONSE (%d bytes)", len);
                    break;
                }
                combustion_controler_info_t s = {
                    .ext_temp        = (int16_t)(((uint16_t)payload[0] << 8) | payload[1]),
                    .exhaust_temp    = (int16_t)(((uint16_t)payload[2] << 8) | payload[3]),
                    .vent_pct        = payload[4],
                    .fire_pct        = payload[5],
                    .combustion_state = payload[6],
                };
                ESP_LOGD(TAG, "STATUS ext=%d.%d exhaust=%d.%d vent=%d%% fire=%d%% comb=%d",
                         s.ext_temp / 10, abs(s.ext_temp % 10),
                         s.exhaust_temp / 10, abs(s.exhaust_temp % 10),
                         s.vent_pct, s.fire_pct, s.combustion_state);
                if (status_cb) status_cb(&s);
                xSemaphoreGive(resp_sem);
                break;
            }
            case MSG_DTDT_RESPONSE: {
                if (len < 12) {
                    ESP_LOGW(TAG, "Short DTDT_RESPONSE (%d bytes)", len);
                    break;
                }
                dTdt_data_t d = {
                    .dTdt_10s        = (int16_t)(((uint16_t)payload[0]  << 8) | payload[1]),
                    .dTdt_20s        = (int16_t)(((uint16_t)payload[2]  << 8) | payload[3]),
                    .dTdt_30s        = (int16_t)(((uint16_t)payload[4]  << 8) | payload[5]),
                    .sliding_max_10s = (int16_t)(((uint16_t)payload[6]  << 8) | payload[7]),
                    .sliding_max_20s = (int16_t)(((uint16_t)payload[8]  << 8) | payload[9]),
                    .sliding_max_30s = (int16_t)(((uint16_t)payload[10] << 8) | payload[11]),
                };
                if (dTdt_cb) dTdt_cb(&d);
                xSemaphoreGive(resp_sem);
                break;
            }
            default:
                ESP_LOGW(TAG, "Unknown msg_id 0x%02x", msg_id);
                break;
        }
    }
}

/* ---------- Public API ---------- */

void comm_set_status_cb(comm_status_cb_t cb)
{
    status_cb = cb;
}

static void time_retry_cb(void *arg)
{
    if (!time_sync_pending) return;

    if (++time_retry_count <= TIME_RETRY_MAX) {
        enqueue_req(MSG_TIME_REQUEST);
        ESP_LOGI(TAG, "Time sync retry %d/%d", time_retry_count, TIME_RETRY_MAX);
        esp_timer_start_once(time_retry_timer, TIME_RETRY_US);
    } else {
        ESP_LOGW(TAG, "Time sync: no response after %d retries", TIME_RETRY_MAX);
    }
}

esp_err_t comm_init(comm_time_cb_t on_time_received)
{
    time_cb   = on_time_received;
    ack_queue = xQueueCreate(1, sizeof(ack_result_t));
    req_queue = xQueueCreate(COMM_REQ_QUEUE_DEPTH, sizeof(uint8_t));
    resp_sem  = xSemaphoreCreateBinary();
    tx_mutex  = xSemaphoreCreateMutex();

    const esp_timer_create_args_t retry_args = {
        .callback = time_retry_cb,
        .name     = "time_retry",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&retry_args, &time_retry_timer), TAG, "time retry timer create failed");

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
    xTaskCreate(tx_task, "comm_tx", 4096, NULL, 5, NULL);

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
    time_sync_pending = true;
    time_retry_count  = 0;
    esp_timer_stop(time_retry_timer);
    enqueue_req(MSG_TIME_REQUEST);
    esp_timer_start_once(time_retry_timer, TIME_RETRY_US);
    return ESP_OK;
}

esp_err_t comm_request_status(void)
{
    enqueue_req(MSG_STATUS_REQUEST);
    return ESP_OK;
}

void comm_set_dTdt_cb(comm_dTdt_cb_t cb)
{
    dTdt_cb = cb;
}

esp_err_t comm_request_dTdt(void)
{
    enqueue_req(MSG_DTDT_REQUEST);
    return ESP_OK;
}
