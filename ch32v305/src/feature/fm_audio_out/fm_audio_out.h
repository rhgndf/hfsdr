#ifndef FM_AUDIO_OUT_H
#define FM_AUDIO_OUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Runtime gate required by plan: FM path runs only when enabled. */
extern bool enable_fm_audio_out;

void fm_audio_out_init(void);
void fm_audio_out_set_enabled(bool enabled);
bool fm_audio_out_get_enabled(void);

/*
 * Process one I2S DMA chunk (16-bit words in groups of 4 per IQ frame).
 * Returns true when chunk was consumed by FM path, false when caller should
 * keep baseline/raw forwarding behavior.
 */
bool fm_audio_out_process_i2s_words_isr(volatile uint16_t const *src_words, size_t word_count);

#endif
