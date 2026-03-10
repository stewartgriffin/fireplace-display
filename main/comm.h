#pragma once

#include <time.h>
#include "esp_err.h"

/* Called from the RX task when a valid TIME_RESPONSE is received.
   The callback should be quick — post to a queue if heavier work is needed. */
typedef void (*comm_time_cb_t)(const struct tm *t);

/* Initialise the RS485 UART, start the RX task, and schedule periodic time polling.
   Sends an immediate TIME_REQUEST on startup. */
esp_err_t comm_init(comm_time_cb_t on_time_received);

/* Send a command to the STM32 and block until ACK/NACK or timeout (~300 ms).
   Returns ESP_OK on ACK, ESP_FAIL on NACK, ESP_ERR_TIMEOUT if no response. */
esp_err_t comm_fireplace_enable(void);
esp_err_t comm_fireplace_disable(void);
esp_err_t comm_ventilation_enable(void);
esp_err_t comm_ventilation_disable(void);

/* Send a TIME_REQUEST and return immediately.
   The response is delivered asynchronously via the on_time_received callback. */
esp_err_t comm_request_time(void);
