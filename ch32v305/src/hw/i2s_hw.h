#ifndef I2S_HW_H
#define I2S_HW_H

#include <stdbool.h>
#include <stdint.h>

#include "debug.h"

/*
 * SPI2 in I2S slave RX: stereo 24-bit Philips in 32-bit channel frames.
 * WS (PB12) and CK (PB13) are supplied by an external I2S controller.
 * SD (PB15) receives serial data from an external I2S ADC / codec.
 * PC6 is reserved for an alternate 24 MHz clock output from TIM8_CH1, so the
 * SPI2 MCK pin is intentionally left unused here.
 *
 * RX uses DMA1 Channel4 in circular mode. DMA half/full-transfer interrupts
 * increment a cumulative word counter and hand each completed half-buffer
 * chunk straight into the TinyUSB microphone FIFO.
 */

void i2s_hw_init(void);
void i2s_hw_deinit(void);
void i2s_hw_enable(FunctionalState state);
[[nodiscard]] bool i2s_needs_reset(void);
[[nodiscard]] uint32_t i2s_hw_rx_word_count(void);

#endif
