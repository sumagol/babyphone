#include "hw_codec.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "hw_codec";

#define I2C_MASTER_SCL_IO 48
#define I2C_MASTER_SDA_IO 47
#define I2C_MASTER_FREQ_HZ 100000
#define ES8311_I2C_ADDR 0x18

// Helper to write PMIC register
static void pmic_write_reg(i2c_master_dev_handle_t handle, uint8_t reg, uint8_t val) {
    uint8_t data[2] = {reg, val};
    esp_err_t err = i2c_master_transmit(handle, data, 2, pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PMIC Write Failed (Reg: 0x%02X, Val: 0x%02X) - Err: %s", reg, val, esp_err_to_name(err));
    }
}

// Write only bit modification
static void pmic_bit_op(i2c_master_dev_handle_t handle, uint8_t reg, uint8_t bit_mask, bool set) {
    // Since reading crashes the I2C on this specific board state, we'll write known good defaults with the bit modified.
    // For M5PM1 GPIO control (0x16, 0x10, 0x13, 0x11), the default values are typically 0x00 or don't care for other pins if not used.
    // We will just blindly set the exact bit. (This is a workaround for the I2C panic).
    static uint8_t reg_cache[256] = {0}; // Assume defaults are 0x00 for these specific GPIO config regs
    if (set) {
        reg_cache[reg] |= bit_mask;
    } else {
        reg_cache[reg] &= ~bit_mask;
    }
    pmic_write_reg(handle, reg, reg_cache[reg]);
    vTaskDelay(pdMS_TO_TICKS(5));
}

esp_err_t hw_codec_init(void)
{
    ESP_LOGI(TAG, "Initializing I2C Master bus (Simplified Write-Only)...");

    i2c_master_bus_config_t i2c_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = -1,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false, // Rely on external pullups
    };
    
    i2c_master_bus_handle_t bus_handle;
    esp_err_t ret = i2c_new_master_bus(&i2c_bus_config, &bus_handle);
    if (ret != ESP_OK) return ret;

    i2c_device_config_t pmic_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x6E,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    i2c_master_dev_handle_t pmic_handle;
    if (i2c_master_bus_add_device(bus_handle, &pmic_cfg, &pmic_handle) == ESP_OK) {
        ESP_LOGI(TAG, "Sending blind writes to PMIC to enable LCD Power...");
        
        // Disable I2C sleep mode
        pmic_write_reg(pmic_handle, 0x09, 0x00);
        vTaskDelay(pdMS_TO_TICKS(50));

        // Configure GPIO2 for LCD Power (bit 2)
        pmic_bit_op(pmic_handle, 0x16, (1 << 2), false); // GPIO function
        pmic_bit_op(pmic_handle, 0x10, (1 << 2), true);  // Output mode
        pmic_bit_op(pmic_handle, 0x13, (1 << 2), false); // Push-pull
        pmic_bit_op(pmic_handle, 0x11, (1 << 2), true);  // Output HIGH (Enable LCD)

        ESP_LOGI(TAG, "LCD Power Enabled via PMIC.");
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

    ESP_LOGI(TAG, "Configuring ES8311 for Microphone input...");
    // M5StickS3 microphone config based on M5Unified
    struct { uint8_t reg; uint8_t val; } es8311_cfg[] = {
        { 0x00, 0x80 }, // RESET / CSM POWER ON
        { 0x01, 0xBA }, // CLOCK_MANAGER / MCLK=BCLK
        { 0x02, 0x18 }, // CLOCK_MANAGER / MULT_PRE=3
        { 0x0D, 0x01 }, // SYSTEM / Power up analog circuitry
        { 0x0E, 0x02 }, // SYSTEM / Enable analog PGA, enable ADC modulator
        { 0x14, 0x10 }, // ADC_REG14 / select Mic1p-Mic1n / PGA GAIN (minimum)
        { 0x17, 0xFF }, // ADC_REG17 / ADC_VOLUME (MAXGAIN)
        { 0x1C, 0x6A }  // ADC_REG1C / ADC Equalizer bypass, cancel DC offset in digital domain
    };

    for (int i = 0; i < sizeof(es8311_cfg) / sizeof(es8311_cfg[0]); i++) {
        uint8_t data[2] = {es8311_cfg[i].reg, es8311_cfg[i].val};
        esp_err_t err = i2c_master_transmit(es8311_handle, data, 2, pdMS_TO_TICKS(100));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ES8311 Write Failed (Reg: 0x%02X) - Err: %s", es8311_cfg[i].reg, esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    
    ESP_LOGI(TAG, "ES8311 I2C setup complete. Deleting hardware I2C driver to free pins for Software I2C PMIC driver...");
    
    // Completely destroy the I2C driver to return pins 47 and 48 back to standard GPIO mode
    i2c_del_master_bus(bus_handle);

    return ESP_OK;
}
