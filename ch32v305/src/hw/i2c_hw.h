#ifndef I2C_HW_H
#define I2C_HW_H

#include <stdint.h>
#include "debug.h"

void i2c_hw_init(void);
ErrorStatus i2c_hw_write_register(uint8_t addr_7bit, uint8_t reg, uint8_t value);
ErrorStatus i2c_hw_read_register(uint8_t addr_7bit, uint8_t reg, uint8_t *value);

#endif
