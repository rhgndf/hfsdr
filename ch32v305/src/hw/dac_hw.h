#ifndef DAC_HW_H
#define DAC_HW_H

#include <stdint.h>

#include "ch32v30x.h"

void dac_hw_init(void);

/* 12-bit codes (0..4095) written to DAC DHR; Vout ≈ Vref * code / 4096 */
void dac_hw_set_channel1_12(uint16_t value);
void dac_hw_set_channel2_12(uint16_t value);
/* Same code on DAC1 (PA4) and DAC2 (PA5). */
void dac_hw_set_both_12(uint16_t value);

/* TIM7-triggered dual-DAC stream via DMA2 Channel3, same sample on both channels. */
void dac_hw_stream_noise_start(uint32_t sample_rate_hz);
void dac_hw_stream_sine_start(uint32_t sine_freq_hz, uint32_t sample_rate_hz);
void dac_hw_stream_stop(void);
[[nodiscard]] uint32_t dac_hw_tx_frame_count(void);

/*
 * Square wave on DAC1 (PA4) and DAC2 (PA5), same waveform: TIM6 toggles both between
 * low_12 and high_12.
 * freq_hz is the full square frequency (each half-period = 1/(2*freq_hz)).
 * Pass freq_hz == 0 to stop (timer off; both DACs held at low_12).
 */
void dac_hw_square_wave_start(uint32_t freq_hz, uint16_t low_12, uint16_t high_12);
void dac_hw_square_wave_stop(void);

#endif
