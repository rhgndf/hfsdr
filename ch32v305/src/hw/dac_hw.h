#ifndef DAC_HW_H
#define DAC_HW_H

#include <stdint.h>

#include "ch32v30x.h"

void dac_hw_init(void);

/* 12-bit codes (0..4095) written to DAC DHR; Vout ≈ Vref * code / 4096 */
void dac_hw_set_channel1_12(uint16_t value);
void dac_hw_set_channel2_12(uint16_t value);

/*
 * Square wave on DAC channel 1 (PA4): TIM6 update toggles between low_12 and high_12.
 * freq_hz is the full square frequency (each half-period = 1/(2*freq_hz)).
 * Pass freq_hz == 0 to stop (timer off, DAC1 held at low_12).
 */
void dac_hw_square_wave_start(uint32_t freq_hz, uint16_t low_12, uint16_t high_12);
void dac_hw_square_wave_stop(void);

#endif
