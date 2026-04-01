#ifndef DAC_HW_SINE_TEST_H
#define DAC_HW_SINE_TEST_H

#include <stdint.h>

/*
 * Thin wrappers over dac_hw's TIM7-triggered dual-DAC DMA stream.
 * Same sample is sent to DAC1 (PA4) and DAC2 (PA5).
 */
void dac_hw_sine_test_start(uint32_t sine_freq_hz, uint32_t sample_rate_hz);
void dac_hw_sine_test_stop(void);

/* White / static noise (xorshift32), full 12-bit amplitude. */
void dac_hw_static_noise_start(uint32_t sample_rate_hz);
void dac_hw_static_noise_stop(void);

#endif
