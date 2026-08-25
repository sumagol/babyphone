#pragma once
#include <stdint.h>
#include <stdbool.h>

// Initializes the Software I2C GPIOs
void sw_i2c_init(int sda_pin, int scl_pin);

// Reads a single byte from a specific register on an I2C device
// Returns true on success, false if the device did not ACK
bool sw_i2c_read_reg(uint8_t dev_addr, uint8_t reg_addr, uint8_t *data_out);
