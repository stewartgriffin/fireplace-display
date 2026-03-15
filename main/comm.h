#pragma once

#include <stdint.h>
#include <time.h>
#include "esp_err.h"

/* Called from the RX task when a valid TIME_RESPONSE is received.
   The callback should be quick — post to a queue if heavier work is needed. */
typedef void (*comm_time_cb_t)(const struct tm *t);

/* Status data returned by the controller in response to a STATUS_REQUEST. */
typedef struct {
    int16_t ext_temp;        /* external temperature, °C × 10  (e.g. 215 = 21.5 °C) */
    int16_t exhaust_temp;    /* exhaust / flue temperature, °C × 10                  */
    uint8_t vent_pct;        /* ventilation damper opening, 0–100 %                  */
    uint8_t fire_pct;        /* fireplace damper opening, 0–100 %                    */
    uint8_t combustion_state;/* combustion state (controller-defined enum)            */
} combustion_controler_info_t;

/* Called from the RX task when a valid STATUS_RESPONSE is received. */
typedef void (*comm_status_cb_t)(const combustion_controler_info_t *status);

/* Initialise the RS485 UART, start the RX task, and schedule periodic time polling.
   Sends an immediate TIME_REQUEST on startup. */
esp_err_t comm_init(comm_time_cb_t on_time_received);

/* Register a callback for STATUS_RESPONSE messages.
   May be called before or after comm_init; pass NULL to deregister. */
void comm_set_status_cb(comm_status_cb_t cb);

/* Send a command to the STM32 and block until ACK/NACK or timeout (~300 ms).
   Returns ESP_OK on ACK, ESP_FAIL on NACK, ESP_ERR_TIMEOUT if no response. */
esp_err_t comm_fireplace_enable(void);
esp_err_t comm_fireplace_disable(void);
esp_err_t comm_ventilation_enable(void);
esp_err_t comm_ventilation_disable(void);

/* Send a TIME_REQUEST and return immediately.
   The response is delivered asynchronously via the on_time_received callback. */
esp_err_t comm_request_time(void);

/* Send a STATUS_REQUEST and return immediately.
   The response is delivered asynchronously via the comm_status_cb_t callback. */
esp_err_t comm_request_status(void);
