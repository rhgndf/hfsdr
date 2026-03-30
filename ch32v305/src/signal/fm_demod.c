#include "signal/fm_demod.h"

#include <stddef.h>
#include <stdint.h>

/* DC blocker: leaky integrator; higher shift = slower tracking (remove DC, keep audio). */
#define FM_DC_SHIFT 10

/* Discriminator output scaling before audio filtering (tune for your IF level). */
#define FM_DISC_SHIFT 18

/* Single-pole low-pass on demod (post-discriminator). */
#define FM_LP_SHIFT 4

/* Simple de-emphasis / extra rolloff (shift 5 ≈ ~1.5 kHz ballpark @ 48 kHz). */
#define FM_DEEMP_SHIFT 5

static int32_t sat_add_12(int32_t x)
{
    if(x < 0)
    {
        return 0;
    }
    if(x > 4095)
    {
        return 4095;
    }
    return x;
}

void fm_demod_init(fm_demod_state_t *st)
{
    if(st == NULL)
    {
        return;
    }
    st->i_dc = 0;
    st->q_dc = 0;
    st->i_prev = 0;
    st->q_prev = 0;
    st->audio_lp = 0;
    st->audio_deemp = 0;
}

uint16_t fm_demod_process_iq(fm_demod_state_t *st, int16_t i_samp, int16_t q_samp)
{
    int32_t I;
    int32_t Q;
    int64_t pd;
    int32_t d;
    int32_t a;

    if(st == NULL)
    {
        return 2048U;
    }

#if FM_DEMOD_SWAP_IQ
    {
        int16_t t = i_samp;
        i_samp = q_samp;
        q_samp = t;
    }
#endif

    I = (int32_t)i_samp;
    Q = (int32_t)q_samp;

    st->i_dc += (I - st->i_dc) >> FM_DC_SHIFT;
    st->q_dc += (Q - st->q_dc) >> FM_DC_SHIFT;
    I -= st->i_dc;
    Q -= st->q_dc;

    pd = (int64_t)I * (int64_t)st->q_prev - (int64_t)Q * (int64_t)st->i_prev;
    st->i_prev = I;
    st->q_prev = Q;

    d = (int32_t)(pd >> FM_DISC_SHIFT);

    st->audio_lp += (d - st->audio_lp) >> FM_LP_SHIFT;
    st->audio_deemp += (st->audio_lp - st->audio_deemp) >> FM_DEEMP_SHIFT;

    a = 2048 + st->audio_deemp;
    return (uint16_t)sat_add_12(a);
}
