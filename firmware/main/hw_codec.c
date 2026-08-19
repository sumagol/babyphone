#include "hw_codec.h"
#include "esp_log.h"
#include "driver/i2c_master.h"

static const char *TAG = "hw_codec";

#define I2C_MASTER_SCL_IO 48
#define I2C_MASTER_SDA_IO 47
#define I2C_MASTER_FREQ_HZ 100000
#define ES8311_I2C_ADDR 0x18

esp_err_t hw_codec_init(void)
{
    ESP_LOGI(TAG, "Initializing I2C Master bus...");

    i2c_master_bus_config_t i2c_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = -1, // Auto select
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    
    i2c_master_bus_handle_t bus_handle;
    esp_err_t ret = i2c_new_master_bus(&i2c_bus_config, &bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2C master bus");
        return ret;
    }

    ESP_LOGI(TAG, "Adding ES8311 device to I2C bus...");
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8311_I2C_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };

    i2c_master_dev_handle_t es8311_handle;
    ret = i2c_master_bus_add_device(bus_handle, &dev_cfg, &es8311_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ES8311 device to I2C bus");
        return ret;
    }

    // TODO: Write ES8311 specific initialization registers
    // (e.g. power up, clock config, ADC/PGA settings, I2S format)
    ESP_LOGI(TAG, "ES8311 I2C setup complete. Register config pending.");

    return ESP_OK;
}
