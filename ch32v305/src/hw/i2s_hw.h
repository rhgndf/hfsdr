#ifndef I2S_HW_H
#define I2S_HW_H

#include <stddef.h>
#include <stdint.h>
#include "debug.h"

/*
 * SPI2 in I2S master RX: 16-bit Philips, 48 kHz (see i2s_hw_init).
 * SD (PB15) receives serial data from an external I2S ADC / codec.
 * RX uses DMA1 channel 4 circular buffer; i2s_hw_try_receive_* reads from that buffer.
 */

void i2s_hw_init(void);
void i2s_hw_enable(FunctionalState state);
void i2s_hw_send_u16(uint16_t sample);
uint16_t i2s_hw_receive_u16(void);
ErrorStatus i2s_hw_try_receive_u16(uint16_t *sample);

/* Blocking: read exactly n contiguous samples (waits for RXNE each time). */
void i2s_hw_receive_burst_blocking(uint16_t *buf, size_t n);

/* Non-blocking: drain RX FIFO into buf; returns number of samples stored. */
size_t i2s_hw_receive_drain_try(uint16_t *buf, size_t max_n);

#endif
