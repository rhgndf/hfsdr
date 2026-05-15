#include "feature/fm_audio_out/fm_audio_out.h"

extern "C" {
#include "hw/dac.h"
}

#include "utils/dsp.h"

#include <cstring>
#include <numbers>
#include <tuple>

constexpr uint32_t FM_AUDIO_OUT_GAIN_DEFAULT_Q16 = 0UL;
/* alpha = exp(-1/(Fs*tau)) in Q31 at the post-upsample audio rate of 192 kHz. */
constexpr int32_t FM_DEEMPH_ALPHA_50US_Q31  = 1935044054;  /* tau=50us,  alpha=0.901080 */
constexpr int32_t FM_DEEMPH_ALPHA_300US_Q31 = 2100413000;  /* tau=300us, alpha=0.978078 */
constexpr uint8_t FM_IQ_CIC_MAX_TAPS = 16U;
constexpr uint8_t FM_AUDIO_CIC_TAPS = 4U;
constexpr uint8_t FM_DAC_UPSAMPLE = 1U;

/* State mirrors the fixed-point path: optional IQ CIC -> discriminator -> audio CIC -> deemph -> DAC DMA. */
static int32_t s_i_prev = 0;
static int32_t s_q_prev = 0;
static int32_t s_dac_sd_error_q19 = 0;
static volatile uint32_t s_audio_gain_q16 = FM_AUDIO_OUT_GAIN_DEFAULT_Q16;
static CICFilter<int32_t, FM_AUDIO_CIC_TAPS> s_audio_cic;
static CICComplexFilter<int32_t, FM_IQ_CIC_MAX_TAPS> s_iq_cic;
static SinglePoleIIR<int32_t, 31> s_deemph;
static volatile fm_audio_out_mode_t s_mode = FM_AUDIO_OUT_MODE_WBFM;

/* ~10–20 ms history at ~48–96 kHz equivalent (one sample per IQ frame). Power of two. */
constexpr size_t FM_WAVE_RING_WORDS = FM_AUDIO_WAVEFORM_RING_MAX_SAMPLES;
static_assert((FM_WAVE_RING_WORDS & (FM_WAVE_RING_WORDS - 1U)) == 0U,
              "wave ring size must be power of two");
static uint16_t s_wave_ring[FM_WAVE_RING_WORDS];
static volatile uint32_t s_wave_writes;

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

template<fm_audio_out_mode_t MODE>
static uint16_t fm_q31_to_dac12(int32_t y_q31, uint32_t gain_q16)
{
    /* NBFM peak deviation is ~25x smaller than WBFM, so boost the discriminator output by 32x
       (clamp at the DAC rails handles the rare loud transient). */
    constexpr uint8_t shift = (MODE == FM_AUDIO_OUT_MODE_NBFM) ? 13U : 17U;
    int64_t audio_q19 = ((int64_t)y_q31 * (int64_t)gain_q16) >> shift;
    int64_t shaped_q19 = ((int64_t)2048 << 19) + audio_q19 + (int64_t)s_dac_sd_error_q19;
    int64_t dac64 = (shaped_q19 + ((int64_t)1 << 18)) >> 19;
    /* dac is provably in [0, 4095] after the clamp, which lets dac_pack_dual_12 skip its
     * own range check. Residual is always computed and masked to 0 when saturation
     * occurred, preventing SD-integrator windup at the rails. */
    int64_t clamped64 = std::clamp<int64_t>(dac64, (int64_t)0, (int64_t)4095);
    int32_t dac = (int32_t)clamped64;
    int32_t residual = (int32_t)(shaped_q19 - (clamped64 << 19));
    int32_t not_sat_mask = -(int32_t)(clamped64 == dac64);
    s_dac_sd_error_q19 = residual & not_sat_mask;
    return (uint16_t)dac;
}

static void fm_audio_out_reset_filters(void)
{
    s_dac_sd_error_q19 = 0;
    s_audio_cic.reset();
    s_iq_cic.reset();
    s_deemph.reset();
}

extern "C" void fm_audio_out_set_gain(uint32_t gain_q16)
{
    s_audio_gain_q16 = gain_q16;
}

extern "C" void fm_audio_out_set_mode(fm_audio_out_mode_t mode)
{
    if(mode >= FM_AUDIO_OUT_MODE_COUNT)
    {
        mode = FM_AUDIO_OUT_MODE_WBFM;
    }

    if(mode == s_mode)
    {
        return;
    }

    s_mode = mode;
    fm_audio_out_reset_filters();
}

extern "C" fm_audio_out_mode_t fm_audio_out_get_mode(void)
{
    return s_mode;
}

extern "C" void fm_audio_out_init(void)
{
    s_i_prev = 0;
    s_q_prev = 0;
    s_audio_gain_q16 = FM_AUDIO_OUT_GAIN_DEFAULT_Q16;
    s_mode = FM_AUDIO_OUT_MODE_WBFM;
    (void)std::memset(s_wave_ring, 0, sizeof(s_wave_ring));
    s_wave_writes = 0U;
    fm_audio_out_reset_filters();
    dac_hw_stream_fm_start(192000U);
}

template<fm_audio_out_mode_t MODE>
static void fm_audio_out_process_frames(const uint16_t* words, size_t frame_count)
{
    constexpr int32_t deemph_alpha_q31 = (MODE == FM_AUDIO_OUT_MODE_NBFM)
        ? FM_DEEMPH_ALPHA_300US_Q31
        : FM_DEEMPH_ALPHA_50US_Q31;
    /* Snapshot the volatile gain once per ISR call so the inner upsample loop
     * doesn't reload it on every iteration. */
    uint32_t const gain_q16 = s_audio_gain_q16;

    uint32_t const *words32 = (uint32_t const *)(uintptr_t)words;
    for(size_t i = 0U; i < frame_count; ++i)
    {
        uint32_t raw_i = words32[i * 2U];
        uint32_t raw_q = words32[i * 2U + 1U];
        int32_t i_now = (int32_t)((raw_i << 16) | (raw_i >> 16));
        int32_t q_now = (int32_t)((raw_q << 16) | (raw_q >> 16));

        int32_t i_filt;
        int32_t q_filt;
        if constexpr(MODE == FM_AUDIO_OUT_MODE_NBFM)
        {
            std::tie(i_filt, q_filt) = s_iq_cic.push(i_now, q_now);
        }
        else
        {
            i_filt = i_now;
            q_filt = q_now;
        }

        /* mulh-truncate each product before combining: 4 mulh + add/sub vs full 64-bit subtract with borrow.
           on very rare cases when inputs are at the limits, num/den may overflow, but practically that won't happen
         */
        int32_t num = (int32_t)(((int64_t)i_filt * (int64_t)s_q_prev) >> 32)
                    - (int32_t)(((int64_t)q_filt * (int64_t)s_i_prev) >> 32);
        int32_t den = (int32_t)(((int64_t)i_filt * (int64_t)s_i_prev) >> 32)
                    + (int32_t)(((int64_t)q_filt * (int64_t)s_q_prev) >> 32);

        int32_t fm_q31 = -atan2_q29(num, den);
        int32_t audio_cic_q31 = s_audio_cic.push(fm_q31);
        uint16_t const dac12 = fm_q31_to_dac12<MODE>(s_deemph.push(audio_cic_q31, deemph_alpha_q31), gain_q16);
        dac_hw_stream_fm_push_sample_isr(dac12);
        {
            uint32_t const w = s_wave_writes;
            s_wave_ring[w & (FM_WAVE_RING_WORDS - 1U)] = dac12;
            s_wave_writes = w + 1U;
        }

        s_i_prev = i_filt;
        s_q_prev = q_filt;
    }
}

extern "C" bool fm_audio_out_process_i2s_words_isr(volatile uint16_t const *src_words, size_t word_count)
{
    if((src_words == nullptr) || (word_count < 4U))
    {
        return false;
    }

    size_t frame_count = word_count / 4U;
    const uint16_t* words = const_cast<const uint16_t*>(src_words);

    if(s_mode == FM_AUDIO_OUT_MODE_NBFM)
    {
        fm_audio_out_process_frames<FM_AUDIO_OUT_MODE_NBFM>(words, frame_count);
    }
    else
    {
        fm_audio_out_process_frames<FM_AUDIO_OUT_MODE_WBFM>(words, frame_count);
    }

    return true;
}

extern "C" size_t fm_audio_waveform_copy_recent(uint16_t *dst, size_t max_samples)
{
    if((dst == nullptr) || (max_samples == 0U))
    {
        return 0U;
    }

    uint32_t const w = s_wave_writes;
    if(w == 0U)
    {
        return 0U;
    }

    size_t const cap = (w < FM_WAVE_RING_WORDS) ? static_cast<size_t>(w) : FM_WAVE_RING_WORDS;
    size_t const count = (cap < max_samples) ? cap : max_samples;

    uint32_t const idx0 = w - static_cast<uint32_t>(count);
    for(size_t k = 0U; k < count; ++k)
    {
        dst[k] = s_wave_ring[(idx0 + static_cast<uint32_t>(k)) & (FM_WAVE_RING_WORDS - 1U)];
    }

    return count;
}
