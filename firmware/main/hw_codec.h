#pragma once

#include "esp_err.h"

/**
 * @brief Initialize the I2C bus and configure the ES8311 codec.
 * 
 * Uses GPIO 47 for SDA and GPIO 48 for SCL.
 * Configures the ES8311 codec (I2C 0x18) for microphone input.
 * 
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t hw_codec_init(void);
