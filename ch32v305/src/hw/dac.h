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

/* TIM7-triggered dual-DAC stream via DMA2 Channel3, same sample on both channels.
 * The DMA buffer is the SPSC ring: dac_hw_stream_fm_push_sample_isr writes one
 * packed dual-12 word per call, DMA reads them out cyclically at sample_rate_hz.
 * No HT/TC interrupts; producer and consumer rates must be locked to the same clock. */
void dac_hw_stream_fm_start(uint32_t sample_rate_hz);
void dac_hw_stream_fm_push_sample_isr(uint16_t sample);
[[nodiscard]] volatile uint32_t const *dac_hw_stream_ring_samples(void);
void dac_hw_stream_adjust_buffer(void);
void dac_hw_stream_stop(void);

#endif
