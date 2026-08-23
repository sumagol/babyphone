#include "hw_display.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
static const char *TAG = "hw_display";

#define LCD_HOST       SPI3_HOST
#define TFT_MOSI       39
#define TFT_CLK        40
#define TFT_DC         45
#define TFT_RST        21
#define TFT_CS         41
#define TFT_BL         38
#define LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000)
#define LCD_H_RES      240
#define LCD_V_RES      135

static esp_lcd_panel_handle_t panel_handle = NULL;
static void (*flush_ready_cb)(void) = NULL;

static bool on_color_trans_done(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    if (flush_ready_cb) {
        flush_ready_cb();
    }
    return false;
}

void hw_display_set_flush_cb(void (*cb)(void))
{
    flush_ready_cb = cb;
}

esp_err_t hw_display_init(void)
{
    ESP_LOGI(TAG, "Initializing SPI bus for display...");

    spi_bus_config_t buscfg = {
        .sclk_io_num = TFT_CLK,
        .mosi_io_num = TFT_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * sizeof(uint16_t),
    };
    esp_err_t ret = spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "Installing panel IO...");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = TFT_DC,
        .cs_gpio_num = TFT_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_color_trans_done,
    };
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "Installing ST7789 panel driver...");
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = TFT_RST,
        .rgb_endian = LCD_RGB_ENDIAN_RGB,
        .bits_per_pixel = 16,
    };
    ret = esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle);
    if (ret != ESP_OK) return ret;

    esp_lcd_panel_reset(panel_handle);
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    
    // Portrait Mode (135x240)
    // No X/Y swap needed for portrait
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, false));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, false, false));
    // Default hardware gaps for portrait
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 52, 40));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    ESP_LOGI(TAG, "Configuring backlight using LEDC PWM to prevent brownouts...");
    
    // Prepare and then apply the LEDC PWM timer configuration
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .timer_num        = LEDC_TIMER_0,
        .duty_resolution  = LEDC_TIMER_8_BIT, // 0-255
        .freq_hz          = 5000,
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // Prepare and then apply the LEDC PWM channel configuration
    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = LEDC_CHANNEL_0,
        .timer_sel      = LEDC_TIMER_0,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = TFT_BL,
        .duty           = 50, // Approx 20% brightness (50/255)
        .hpoint         = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));

    esp_lcd_panel_disp_on_off(panel_handle, true);
    return ESP_OK;
}

void hw_display_set_backlight(uint8_t brightness)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, brightness);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void hw_display_test_pattern(void)
{
    if (!panel_handle) return;
    
    // Allocate a buffer for a green screen chunk (40 lines)
    int lines_per_chunk = 40;
    size_t chunk_size = LCD_H_RES * lines_per_chunk;
    uint16_t *color_data = (uint16_t *)heap_caps_malloc(chunk_size * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!color_data) {
        ESP_LOGE(TAG, "No memory for test pattern buffer");
        return;
    }

    // Fill with RGB565 green, swapped endianness for SPI
    // Green is 0x07E0, swapped is 0xE007
    for (int i = 0; i < chunk_size; i++) {
        color_data[i] = 0xE007;
    }

    ESP_LOGI(TAG, "Drawing green test pattern in chunks...");
    for (int y = 0; y < LCD_V_RES; y += lines_per_chunk) {
        int height = lines_per_chunk;
        if (y + height > LCD_V_RES) {
            height = LCD_V_RES - y;
        }
        esp_lcd_panel_draw_bitmap(panel_handle, 0, y, LCD_H_RES, y + height, color_data);
    }
    // We intentionally don't free color_data here because esp_lcd_panel_draw_bitmap uses DMA asynchronously.
    // In a real app with a proper render loop, we would use lvgl or an on_color_trans_done callback.
}

esp_lcd_panel_handle_t hw_display_get_panel(void)
{
    return panel_handle;
}
