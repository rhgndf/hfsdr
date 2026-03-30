#ifndef DAC_HW_SINE_TEST_H
#define DAC_HW_SINE_TEST_H

#include <stdint.h>

/*
 * DDS sine on DAC1 (PA4) and DAC2 (PA5), same sample on both.
 * Uses TIM7 at sample_rate_hz; stops any active dac_hw square wave (TIM6) first.
 */
void dac_hw_sine_test_start(uint32_t sine_freq_hz, uint32_t sample_rate_hz);
void dac_hw_sine_test_stop(void);

#endif
