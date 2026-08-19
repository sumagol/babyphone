#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "esp_err.h"

/**
 * @brief Initialize the Network UDP Multicast TX task.
 * 
 * @param rtp_in_buf Ringbuffer handle to read encoded RTP packets from (input).
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t network_tx_init(RingbufHandle_t rtp_in_buf);
