#ifndef DAC_HW_SINE_TEST_H
#define DAC_HW_SINE_TEST_H

#include <stdint.h>

/*
 * TIM7 audio-rate ISR on DAC1 (PA4) and DAC2 (PA5), same sample on both.
 * Stops any active dac_hw square wave (TIM6) first. Only one TIM7 mode at a time.
 */
void dac_hw_sine_test_start(uint32_t sine_freq_hz, uint32_t sample_rate_hz);
void dac_hw_sine_test_stop(void);

/* White / static noise (xorshift32), full 12-bit amplitude. */
void dac_hw_static_noise_start(uint32_t sample_rate_hz);
void dac_hw_static_noise_stop(void);

#endif
