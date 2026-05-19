#ifndef DEMOD_H
#define DEMOD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    DEMODULATION_MODE_WBFM = 0,
    DEMODULATION_MODE_NBFM,
    DEMODULATION_MODE_AM,
    DEMODULATION_MODE_USB,
    DEMODULATION_MODE_LSB,
    DEMODULATION_MODE_COUNT
} demodulation_mode_t;

void demod_init(void);
void demod_set_gain(uint32_t gain_q16);
void demod_set_mode(demodulation_mode_t mode);
demodulation_mode_t demod_get_mode(void);

/*
 * Process one I2S DMA chunk (16-bit words in groups of 4 per IQ frame).
 * Returns true when the chunk was consumed by the demodulator path.
 */
bool demod_process_isr(volatile uint16_t const *src_words, size_t word_count);

#ifdef __cplusplus
}
#endif

#endif
