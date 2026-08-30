#include <stddef.h>
#include <stdint.h>
#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

/**
 * @brief Initialize the I2S hardware interface for reading from the ES8311 codec.
 * 
 * Configures the ESP32-S3 I2S driver for 16kHz, 16-bit Mono via DMA.
 * 
 * @param pcm_out_buf Ringbuffer handle to write raw PCM frames to (output).
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t audio_capture_init(RingbufHandle_t pcm_out_buf);
