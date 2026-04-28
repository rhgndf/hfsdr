#ifndef FM_AUDIO_OUT_H
#define FM_AUDIO_OUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum
{
    FM_AUDIO_OUT_MODE_WBFM = 0,
    FM_AUDIO_OUT_MODE_NBFM,
    FM_AUDIO_OUT_MODE_COUNT
} fm_audio_out_mode_t;

void fm_audio_out_init(void);
void fm_audio_out_set_gain(uint32_t gain_q16);
void fm_audio_out_set_mode(fm_audio_out_mode_t mode);
fm_audio_out_mode_t fm_audio_out_get_mode(void);

/*
 * Process one I2S DMA chunk (16-bit words in groups of 4 per IQ frame).
 * Returns true when chunk was consumed by FM path.
 */
bool fm_audio_out_process_i2s_words_isr(volatile uint16_t const *src_words, size_t word_count);

#endif
