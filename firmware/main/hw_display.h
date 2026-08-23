#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

/**
 * @brief Initialize the ST7789 display and turn on backlight.
 * 
 * Configures the SPI bus and esp_lcd panel driver.
 * 
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t hw_display_init(void);

/**
 * @brief Set the backlight brightness (0-255)
 */
void hw_display_set_backlight(uint8_t brightness);

/**
 * @brief Register a callback to be called when the SPI DMA transfer completes
 */
void hw_display_set_flush_cb(void (*cb)(void));

/**
 * @brief Turn the display screen green to indicate successful boot.
 */
void hw_display_test_pattern(void);

/**
 * @brief Get the initialized LCD panel handle for LVGL integration
 */
esp_lcd_panel_handle_t hw_display_get_panel(void);
