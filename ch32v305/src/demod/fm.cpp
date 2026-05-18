#include "demod/demod_internal.h"

#include "utils/dsp.h"

#include <numbers>
#include <tuple>

namespace
{
/* alpha = exp(-1/(Fs*tau)) in Q31 at the post-upsample audio rate of 192 kHz. */
constexpr int32_t kDeemphAlpha50UsQ31  = 1935044054;  /* tau=50us,  alpha=0.901080 */
constexpr int32_t kDeemphAlpha300UsQ31 = 2100413000;  /* tau=300us, alpha=0.978078 */
constexpr uint8_t kIqCicMaxTaps = 16U;
constexpr uint8_t kAudioCicTaps = 4U;

static int32_t s_i_prev = 0;
static int32_t s_q_prev = 0;
static CICFilter<int32_t, kAudioCicTaps> s_audio_cic;
static CICComplexFilter<int32_t, kIqCicMaxTaps> s_iq_cic;
static SinglePoleIIR<int32_t, 31> s_deemph;

static constexpr int32_t s_pi_q29 = static_cast<int32_t>(std::numbers::pi_v<double> * (1LL << 29) + 0.5);

static constexpr int32_t s_cordic_table[14] = {
    421657428, 248918915, 131521918, 66762579, 33510843,
    16771758,  8387925,   4194219,   2097141,  1048575,
    524288,    262144,    131072,    65536
};

static int32_t atan2_q29(int32_t y, int32_t x)
{
    if((x | y) == 0)
        return 0;

    int32_t angle = 0;
    if(x < 0)
    {
        x = -x;
        y = -y;
        angle = (y <= 0) ? s_pi_q29 : -s_pi_q29;
    }

    x >>= 1;
    y >>= 1;

    for(int i = 0; i < 14; i++)
    {
        int32_t xn = x;
        if(y >= 0)
        {
            x += y >> i;
            y -= xn >> i;
            angle += s_cordic_table[i];
        }
        else
        {
            x -= y >> i;
            y += xn >> i;
            angle -= s_cordic_table[i];
        }
    }

    return angle;
}

template<demodulation_mode_t MODE>
static void fm_process_frames(const uint16_t *words, size_t frame_count, uint32_t gain_q16)
{
    constexpr int32_t deemph_alpha_q31 = (MODE == DEMODULATION_MODE_NBFM)
        ? kDeemphAlpha300UsQ31
        : kDeemphAlpha50UsQ31;
    constexpr uint8_t dac_shift = (MODE == DEMODULATION_MODE_NBFM) ? 13U : 17U;

    uint32_t const *words32 = (uint32_t const *)(uintptr_t)words;
    for(size_t i = 0U; i < frame_count; ++i)
    {
        uint32_t raw_i = words32[i * 2U];
        uint32_t raw_q = words32[i * 2U + 1U];
        int32_t i_now = (int32_t)((raw_i << 16) | (raw_i >> 16));
        int32_t q_now = (int32_t)((raw_q << 16) | (raw_q >> 16));

        int32_t i_filt;
        int32_t q_filt;
        if constexpr(MODE == DEMODULATION_MODE_NBFM)
        {
            std::tie(i_filt, q_filt) = s_iq_cic.push(i_now, q_now);
        }
        else
        {
            i_filt = i_now;
            q_filt = q_now;
        }

        /* mulh-truncate each product before combining: 4 mulh + add/sub vs full 64-bit subtract with borrow.
           On rare full-scale inputs num/den may overflow, but practical ADC samples have enough headroom. */
        int32_t num = (int32_t)(((int64_t)i_filt * (int64_t)s_q_prev) >> 32)
                    - (int32_t)(((int64_t)q_filt * (int64_t)s_i_prev) >> 32);
        int32_t den = (int32_t)(((int64_t)i_filt * (int64_t)s_i_prev) >> 32)
                    + (int32_t)(((int64_t)q_filt * (int64_t)s_q_prev) >> 32);

        int32_t fm_q31 = -atan2_q29(num, den);
        int32_t audio_cic_q31 = s_audio_cic.push(fm_q31);
        uint16_t const dac12 = demod::audio_to_dac12(s_deemph.push(audio_cic_q31, deemph_alpha_q31),
                                                     gain_q16,
                                                     dac_shift);
        dac_hw_stream_fm_push_sample_isr(dac12);

        s_i_prev = i_filt;
        s_q_prev = q_filt;
    }
}
}

namespace demod
{
void fm_reset()
{
    s_i_prev = 0;
    s_q_prev = 0;
    s_audio_cic.reset();
    s_iq_cic.reset();
    s_deemph.reset();
}

void fm_process_wbfm_i2s_words(const uint16_t *words, size_t frame_count, uint32_t gain_q16)
{
    fm_process_frames<DEMODULATION_MODE_WBFM>(words, frame_count, gain_q16);
}

void fm_process_nbfm_i2s_words(const uint16_t *words, size_t frame_count, uint32_t gain_q16)
{
    fm_process_frames<DEMODULATION_MODE_NBFM>(words, frame_count, gain_q16);
}
}
