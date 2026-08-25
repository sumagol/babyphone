#include "sw_i2c.h"
#include "driver/gpio.h"
#include "rom/ets_sys.h"

static int _sda = -1;
static int _scl = -1;

#define I2C_DELAY() ets_delay_us(5) // ~100kHz

static void set_sda(bool high) {
    if (high) {
        gpio_set_direction(_sda, GPIO_MODE_INPUT);
    } else {
        gpio_set_direction(_sda, GPIO_MODE_OUTPUT);
        gpio_set_level(_sda, 0);
    }
}

static void set_scl(bool high) {
    if (high) {
        gpio_set_direction(_scl, GPIO_MODE_INPUT);
        // Wait for clock stretching
        int timeout = 1000;
        while(gpio_get_level(_scl) == 0 && timeout > 0) {
            ets_delay_us(1);
            timeout--;
        }
    } else {
        gpio_set_direction(_scl, GPIO_MODE_OUTPUT);
        gpio_set_level(_scl, 0);
    }
}

static bool get_sda(void) {
    gpio_set_direction(_sda, GPIO_MODE_INPUT);
    return gpio_get_level(_sda);
}

static void i2c_start(void) {
    set_sda(true);
    set_scl(true);
    I2C_DELAY();
    set_sda(false);
    I2C_DELAY();
    set_scl(false);
    I2C_DELAY();
}

static void i2c_stop(void) {
    set_sda(false);
    I2C_DELAY();
    set_scl(true);
    I2C_DELAY();
    set_sda(true);
    I2C_DELAY();
}

static bool i2c_write_byte(uint8_t data) {
    for (int i = 0; i < 8; i++) {
        set_sda((data & 0x80) != 0);
        I2C_DELAY();
        set_scl(true);
        I2C_DELAY();
        set_scl(false);
        I2C_DELAY();
        data <<= 1;
    }
    
    // Read ACK
    set_sda(true);
    I2C_DELAY();
    set_scl(true);
    I2C_DELAY();
    bool ack = !get_sda();
    set_scl(false);
    I2C_DELAY();
    
    return ack;
}

static uint8_t i2c_read_byte(bool send_ack) {
    uint8_t data = 0;
    set_sda(true);
    for (int i = 0; i < 8; i++) {
        data <<= 1;
        I2C_DELAY();
        set_scl(true);
        I2C_DELAY();
        if (get_sda()) data |= 1;
        set_scl(false);
    }
    
    // Send ACK/NACK
    set_sda(!send_ack);
    I2C_DELAY();
    set_scl(true);
    I2C_DELAY();
    set_scl(false);
    I2C_DELAY();
    set_sda(true);
    
    return data;
}

void sw_i2c_init(int sda_pin, int scl_pin) {
    _sda = sda_pin;
    _scl = scl_pin;
    
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT_OUTPUT_OD;
    io_conf.pin_bit_mask = (1ULL << _sda) | (1ULL << _scl);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 1;
    gpio_config(&io_conf);
    
    set_sda(true);
    set_scl(true);
}

bool sw_i2c_read_reg(uint8_t dev_addr, uint8_t reg_addr, uint8_t *data_out) {
    if (_sda < 0 || _scl < 0) return false;
    
    i2c_start();
    if (!i2c_write_byte(dev_addr << 1)) {
        i2c_stop();
        return false;
    }
    
    if (!i2c_write_byte(reg_addr)) {
        i2c_stop();
        return false;
    }
    
    i2c_start();
    if (!i2c_write_byte((dev_addr << 1) | 1)) {
        i2c_stop();
        return false;
    }
    
    *data_out = i2c_read_byte(false); // NACK on last read
    i2c_stop();
    return true;
}
