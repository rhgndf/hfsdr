#ifndef I2C_HW_H
#define I2C_HW_H

#include <stddef.h>
#include <stdint.h>
#include "debug.h"

#ifdef __cplusplus
extern "C" {
#endif

void i2c_hw_init(void);
ErrorStatus i2c_hw_write_register(uint8_t addr_7bit, uint8_t reg, uint8_t value);
/* Write reg then len data bytes in one I2C transaction (Si5351 multisynth blocks, etc.). */
ErrorStatus i2c_hw_write_register_burst(uint8_t addr_7bit, uint8_t reg, const uint8_t *data, size_t len);
ErrorStatus i2c_hw_read_register(uint8_t addr_7bit, uint8_t reg, uint8_t *value);
ErrorStatus i2c_hw_scan_bus_at(uint8_t addr_7bit);

/* 16-bit register address variants (high byte first). For chips like CST328 that
 * use a 16-bit command/register space. data may be NULL when len == 0. */
ErrorStatus i2c_hw_write_register16(uint8_t addr_7bit, uint16_t reg16, const uint8_t *data, size_t len);
ErrorStatus i2c_hw_read_register16(uint8_t addr_7bit, uint16_t reg16, uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif
