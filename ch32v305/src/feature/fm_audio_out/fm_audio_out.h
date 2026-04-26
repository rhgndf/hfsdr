#ifndef FM_AUDIO_OUT_H
#define FM_AUDIO_OUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void fm_audio_out_init(void);

/*
 * Process one I2S DMA chunk (16-bit words in groups of 4 per IQ frame).
 * Returns true when chunk was consumed by FM path.
 */
bool fm_audio_out_process_i2s_words_isr(volatile uint16_t const *src_words, size_t word_count);

#endif
