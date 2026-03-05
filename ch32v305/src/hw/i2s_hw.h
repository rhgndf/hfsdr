#ifndef I2S_HW_H
#define I2S_HW_H

#include <stdint.h>
#include "debug.h"

void i2s_hw_init(void);
void i2s_hw_enable(FunctionalState state);
void i2s_hw_send_u16(uint16_t sample);
uint16_t i2s_hw_receive_u16(void);
ErrorStatus i2s_hw_try_receive_u16(uint16_t *sample);

#endif
