#include "feature/fm_audio_out/fm_audio_out.h"

#include "hw/dac_hw.h"

bool enable_fm_audio_out = false;

/* State mirrors the fixed-point prototype: discriminator -> LPF -> decim -> deemph. */
static int32_t s_i_prev = 0;
static int32_t s_q_prev = 0;
static int32_t s_audio_lp = 0;
static int32_t s_deemph_state = 0;
static uint8_t s_has_prev = 0U;
static uint8_t s_decim_ctr = 0U;

/* alpha = exp(-1/(48000*50e-6)) in Q31 */
static const int32_t s_deemph_alpha_q31 = 1412758477;
static const int32_t s_one_minus_alpha_q31 = (int32_t)(0x7FFFFFFF - 1412758477);

static int32_t sat_i32(int64_t x)
{
    if(x > INT32_MAX)
    {
        return INT32_MAX;
    }
    if(x < INT32_MIN)
    {
        return INT32_MIN;
    }
    return (int32_t)x;
}

static uint16_t fm_q31_to_dac12(int32_t y_q31)
{
    int32_t y_i16 = y_q31 >> 15;              /* -32768..32767 nominal */
    int32_t dac = 2048 + (y_i16 >> 5);        /* conservative gain around mid-rail */
    if(dac < 0)
    {
        return 0U;
    }
    if(dac > 4095)
    {
        return 4095U;
    }
    return (uint16_t)dac;
}

void fm_audio_out_init(void)
{
    s_i_prev = 0;
    s_q_prev = 0;
    s_audio_lp = 0;
    s_deemph_state = 0;
    s_has_prev = 0U;
    s_decim_ctr = 0U;
}

void fm_audio_out_set_enabled(bool enabled)
{
    enable_fm_audio_out = enabled;
    fm_audio_out_init();

    if(enabled)
    {
        /* FM audio path writes direct DAC samples; stop background noise streamer. */
        dac_hw_stream_stop();
    }
    else
    {
        /* Preserve existing baseline behavior when FM path is disabled. */
        dac_hw_stream_noise_start(192000U);
    }
}

bool fm_audio_out_get_enabled(void)
{
    return enable_fm_audio_out;
}

bool fm_audio_out_process_i2s_words_isr(volatile uint16_t const *src_words, size_t word_count)
{
    size_t frame_count;
    size_t i;

    if((!enable_fm_audio_out) || (src_words == 0) || (word_count < 4U))
    {
        return false;
    }

    frame_count = word_count / 4U;
    for(i = 0U; i < frame_count; ++i)
    {
        uint16_t i_hi = src_words[i * 4U];
        uint16_t i_lo = src_words[i * 4U + 1U];
        uint16_t q_hi = src_words[i * 4U + 2U];
        uint16_t q_lo = src_words[i * 4U + 3U];
        int32_t i_now = (int32_t)(((uint32_t)i_hi << 16) | (uint32_t)i_lo);
        int32_t q_now = (int32_t)(((uint32_t)q_hi << 16) | (uint32_t)q_lo);

        if(s_has_prev != 0U)
        {
            int64_t num = (((int64_t)i_now * (int64_t)s_q_prev) - ((int64_t)q_now * (int64_t)s_i_prev)) >> 31;
            int64_t den = (((int64_t)i_now * (int64_t)s_i_prev) + ((int64_t)q_now * (int64_t)s_q_prev)) >> 31;
            int32_t fm_q31;

            if((den > -1024) && (den < 1024))
            {
                den = (den >= 0) ? 1024 : -1024;
            }

            fm_q31 = sat_i32(-((num << 29) / den));
            s_audio_lp = sat_i32((((int64_t)s_audio_lp * 7) + fm_q31) / 8);

            s_decim_ctr++;
            if(s_decim_ctr >= 4U)
            {
                int64_t mixed = ((int64_t)s_one_minus_alpha_q31 * (int64_t)s_audio_lp) +
                                ((int64_t)s_deemph_alpha_q31 * (int64_t)s_deemph_state);
                s_deemph_state = sat_i32(mixed >> 31);
                dac_hw_set_both_12(fm_q31_to_dac12(s_deemph_state));
                s_decim_ctr = 0U;
            }
        }
        else
        {
            s_has_prev = 1U;
        }

        s_i_prev = i_now;
        s_q_prev = q_now;
    }

    return true;
}
