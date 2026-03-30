#ifndef FM_DEMOD_H
#define FM_DEMOD_H

#include <stdint.h>

/*
 * Quadrature FM demodulator for I/Q from TLV320ADC6120 (I2S stereo: L=I, R=Q).
 * Sample rate must match i2s_hw (48 kHz). Uses product discriminator:
 *   d = I[n]*Q[n-1] - Q[n]*I[n-1]  ∝ instantaneous frequency.
 *
 * Set FM_DEMOD_SWAP_IQ to 1 if audio polarity or routing is inverted.
 */

#ifndef FM_DEMOD_SWAP_IQ
#define FM_DEMOD_SWAP_IQ 0
#endif

typedef struct fm_demod_state
{
    int32_t i_dc;
    int32_t q_dc;
    int32_t i_prev;
    int32_t q_prev;
    int32_t audio_lp;
    int32_t audio_deemp;
} fm_demod_state_t;

void fm_demod_init(fm_demod_state_t *st);

/* One stereo sample pair → 12-bit DAC code (0..4095, mid 2048). */
uint16_t fm_demod_process_iq(fm_demod_state_t *st, int16_t i_samp, int16_t q_samp);

#endif
