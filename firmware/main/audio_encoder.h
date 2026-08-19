#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "esp_err.h"

/**
 * @brief Initialize the Opus encoder task.
 * 
 * @param pcm_in_buf Ringbuffer handle to read raw PCM frames from (input).
 * @param rtp_out_buf Ringbuffer handle to write encoded RTP packets to (output).
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t audio_encoder_init(RingbufHandle_t pcm_in_buf, RingbufHandle_t rtp_out_buf);
