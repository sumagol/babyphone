#include "hw_display.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

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
    esp_lcd_panel_init(panel_handle);
    
    // M5StickS3 display orientation specific config
    // When swapped (landscape), the offsets are swapped too: X gap is 40, Y gap is 52
    esp_lcd_panel_swap_xy(panel_handle, true);
    esp_lcd_panel_set_gap(panel_handle, 40, 52);
    esp_lcd_panel_invert_color(panel_handle, true);
    esp_lcd_panel_mirror(panel_handle, false, true);

    ESP_LOGI(TAG, "Configuring backlight... TURNING OFF TO SAVE POWER");
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << TFT_BL
    };
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));
    // Set to 0 to turn off backlight (saves ~100mA to prevent brownouts)
    ESP_ERROR_CHECK(gpio_set_level(TFT_BL, 0));

    esp_lcd_panel_disp_on_off(panel_handle, true);
    return ESP_OK;
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
