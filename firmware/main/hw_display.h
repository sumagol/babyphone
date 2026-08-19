#pragma once

#include "esp_err.h"

/**
 * @brief Initialize the ST7789 display and turn on backlight.
 * 
 * Configures the SPI bus and esp_lcd panel driver.
 * 
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t hw_display_init(void);

/**
 * @brief Turn the display screen green to indicate successful boot.
 */
void hw_display_test_pattern(void);
