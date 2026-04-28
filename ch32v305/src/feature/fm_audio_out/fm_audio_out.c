#include "feature/fm_audio_out/fm_audio_out.h"

#include "hw/dac.h"

#define FM_AUDIO_OUT_GAIN_DEFAULT_Q16 0UL
#define FM_DEEMPH_ALPHA_50US_Q31 1935044054
#define FM_DEEMPH_ALPHA_300US_Q31 2100413000
#define FM_IQ_CIC_MAX_TAPS 16U

/* State mirrors the fixed-point path: optional IQ CIC -> discriminator -> audio CIC -> deemph -> DAC DMA. */
static int32_t s_i_prev = 0;
static int32_t s_q_prev = 0;
static int32_t s_deemph_state = 0;
static int32_t s_dac_sd_error_q19 = 0;
static volatile uint32_t s_audio_gain_q16 = FM_AUDIO_OUT_GAIN_DEFAULT_Q16;
static int32_t s_audio_cic_hist_q31[4] = {0};
static int64_t s_audio_cic_sum_q31 = 0;
static uint8_t s_audio_cic_idx = 0U;
static int32_t s_i_cic_hist[FM_IQ_CIC_MAX_TAPS] = {0};
static int32_t s_q_cic_hist[FM_IQ_CIC_MAX_TAPS] = {0};
static int64_t s_i_cic_sum = 0;
static int64_t s_q_cic_sum = 0;
static uint8_t s_iq_cic_idx = 0U;
static uint8_t s_has_prev = 0U;
static volatile fm_audio_out_mode_t s_mode = FM_AUDIO_OUT_MODE_WBFM;

/* pi-domain constants in Q29 so small-angle gain matches the prior num/den path. */
static constexpr int32_t s_pi_q29 = 1686629713;
static constexpr int32_t s_pi_over_2_q29 = 843314857;

/*
 * Odd 5th-degree minimax (Remez) fit of atan(x) on [0, 1]:
 * atan(x) ~= x * (c1 + x^2 * (c3 + x^2 * c5))
 * Max abs error ~6.1e-4 in atan (vs 1.3e-3 for the previous least-squares fit).
 * Coefficients are stored in Q31 and evaluated with Horner form.
 */
static constexpr int32_t s_atan_c1_q31 = 2137514932;
static constexpr int32_t s_atan_c3_q31 = -619957566;
static constexpr int32_t s_atan_c5_q31 = 170379294;

static int64_t clamp_i64(int64_t x, int64_t lo, int64_t hi)
{
    if(x < lo)
    {
        return lo;
    }
    if(x > hi)
    {
        return hi;
    }
    return x;
}

static int32_t sat_i32(int64_t x)
{
    return (int32_t)clamp_i64(x, INT32_MIN, INT32_MAX);
}

static uint8_t fm_iq_cic_shift(void)
{
    return 4U;
}

static void fm_iq_cic_push(int32_t i_now, int32_t q_now, int32_t *i_filt, int32_t *q_filt)
{
    if(s_mode != FM_AUDIO_OUT_MODE_NBFM)
    {
        *i_filt = i_now;
        *q_filt = q_now;
        return;
    }

    int32_t old_i = s_i_cic_hist[s_iq_cic_idx];
    int32_t old_q = s_q_cic_hist[s_iq_cic_idx];
    uint8_t shift = fm_iq_cic_shift();

    s_i_cic_hist[s_iq_cic_idx] = i_now;
    s_q_cic_hist[s_iq_cic_idx] = q_now;
    s_iq_cic_idx = (s_iq_cic_idx + 1U) & (FM_IQ_CIC_MAX_TAPS - 1U);
    s_i_cic_sum += (int64_t)i_now - (int64_t)old_i;
    s_q_cic_sum += (int64_t)q_now - (int64_t)old_q;

    *i_filt = (int32_t)(s_i_cic_sum >> shift);
    *q_filt = (int32_t)(s_q_cic_sum >> shift);
}

static int32_t fm_audio_cic4_q31(int32_t x_q31)
{
    int32_t old_q31 = s_audio_cic_hist_q31[s_audio_cic_idx];

    s_audio_cic_hist_q31[s_audio_cic_idx] = x_q31;
    s_audio_cic_idx = (s_audio_cic_idx + 1U) & 3U;
    s_audio_cic_sum_q31 += (int64_t)x_q31 - (int64_t)old_q31;

    return (int32_t)(s_audio_cic_sum_q31 >> 2);
}

static uint32_t abs_i32(int32_t x)
{
    return (x < 0) ? (uint32_t)(-(int64_t)x) : (uint32_t)x;
}

static int32_t atan_0_1_q29(uint32_t x_q31)
{
    uint64_t x2_q31 = ((uint64_t)x_q31 * (uint64_t)x_q31) >> 31;
    int64_t acc_q31 = s_atan_c5_q31;

    acc_q31 = (int64_t)s_atan_c3_q31 + ((acc_q31 * (int64_t)x2_q31) >> 31);
    acc_q31 = (int64_t)s_atan_c1_q31 + ((acc_q31 * (int64_t)x2_q31) >> 31);

    return (int32_t)((((int64_t)x_q31 * acc_q31) >> 31) >> 2);
}

static int32_t atan2_q29(int32_t y, int32_t x)
{
    uint32_t ay = abs_i32(y);
    uint32_t ax = abs_i32(x);
    int32_t base_q29;
    uint32_t ratio_q31;

    if((ax == 0U) && (ay == 0U))
    {
        return 0;
    }

    if(ax >= ay)
    {
        ratio_q31 = (uint32_t)(((uint64_t)ay << 31) / ax);
        base_q29 = atan_0_1_q29(ratio_q31);
    }
    else
    {
        ratio_q31 = (uint32_t)(((uint64_t)ax << 31) / ay);
        base_q29 = s_pi_over_2_q29 - atan_0_1_q29(ratio_q31);
    }

    if(x < 0)
    {
        base_q29 = s_pi_q29 - base_q29;
    }

    return (y < 0) ? -base_q29 : base_q29;
}

static uint16_t fm_q31_to_dac12(int32_t y_q31)
{
    uint32_t gain_q16 = s_audio_gain_q16;
    int64_t audio_q19 = ((int64_t)y_q31 * (int64_t)gain_q16) >> 17;
    int64_t shaped_q19 = ((int64_t)2048 << 19) + audio_q19 + (int64_t)s_dac_sd_error_q19;
    int64_t clamped_q19 = clamp_i64(shaped_q19, 0, (int64_t)4095 << 19);
    int32_t dac;

    /* Clamp before quantizing so the error integrator does not wind up at the rails. */
    if(clamped_q19 != shaped_q19)
    {
        s_dac_sd_error_q19 = 0;
        return (uint16_t)(clamped_q19 >> 19);
    }

    dac = (int32_t)((shaped_q19 + ((int64_t)1 << 18)) >> 19);
    s_dac_sd_error_q19 = (int32_t)(shaped_q19 - ((int64_t)dac << 19));

    return (uint16_t)dac;
}

static void fm_audio_out_reset_filters(void)
{
    uint8_t i;

    s_deemph_state = 0;
    s_dac_sd_error_q19 = 0;
    s_audio_cic_hist_q31[0] = 0;
    s_audio_cic_hist_q31[1] = 0;
    s_audio_cic_hist_q31[2] = 0;
    s_audio_cic_hist_q31[3] = 0;
    s_audio_cic_sum_q31 = 0;
    s_audio_cic_idx = 0U;
    for(i = 0U; i < FM_IQ_CIC_MAX_TAPS; ++i)
    {
        s_i_cic_hist[i] = 0;
        s_q_cic_hist[i] = 0;
    }
    s_i_cic_sum = 0;
    s_q_cic_sum = 0;
    s_iq_cic_idx = 0U;
}

void fm_audio_out_set_gain(uint32_t gain_q16)
{
    s_audio_gain_q16 = gain_q16;
}

void fm_audio_out_set_mode(fm_audio_out_mode_t mode)
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

fm_audio_out_mode_t fm_audio_out_get_mode(void)
{
    return s_mode;
}

void fm_audio_out_init(void)
{
    s_i_prev = 0;
    s_q_prev = 0;
    s_audio_gain_q16 = FM_AUDIO_OUT_GAIN_DEFAULT_Q16;
    s_mode = FM_AUDIO_OUT_MODE_WBFM;
    s_has_prev = 0U;
    fm_audio_out_reset_filters();
    dac_hw_stream_fm_start(192000U);
}

bool fm_audio_out_process_i2s_words_isr(volatile uint16_t const *src_words, size_t word_count)
{
    size_t frame_count;
    size_t i;

    if((src_words == 0) || (word_count < 4U))
    {
        return false;
    }

    frame_count = word_count / 4U;
    const uint16_t* words = (const uint16_t*)src_words;
    for(i = 0U; i < frame_count; ++i)
    {
        uint16_t i_hi = words[i * 4U + 0U];
        uint16_t i_lo = words[i * 4U + 1U];
        uint16_t q_hi = words[i * 4U + 2U];
        uint16_t q_lo = words[i * 4U + 3U];
        int32_t i_now = (int32_t)(((uint32_t)i_hi << 16) | (uint32_t)i_lo);
        int32_t q_now = (int32_t)(((uint32_t)q_hi << 16) | (uint32_t)q_lo);
        int32_t i_filt = 0;
        int32_t q_filt = 0;

        fm_iq_cic_push(i_now, q_now, &i_filt, &q_filt);

        if(s_has_prev != 0U)
        {
            /* mulh-truncate each product before combining: 4 mulh + add/sub vs full 64-bit subtract with borrow.
               on very rare cases when inputs are at the limits, num/den may overflow, but practically that won't happen
             */
            int32_t num = (int32_t)(((int64_t)i_filt * (int64_t)s_q_prev) >> 32)
                        - (int32_t)(((int64_t)q_filt * (int64_t)s_i_prev) >> 32);
            int32_t den = (int32_t)(((int64_t)i_filt * (int64_t)s_i_prev) >> 32)
                        + (int32_t)(((int64_t)q_filt * (int64_t)s_q_prev) >> 32);
            int32_t fm_q31;
            int32_t audio_cic_q31;
            int32_t deemph_alpha_q31;
            int64_t mixed;

            fm_q31 = -atan2_q29(num, den);
            audio_cic_q31 = fm_audio_cic4_q31(fm_q31);
            if(s_mode == FM_AUDIO_OUT_MODE_NBFM)
            {
                deemph_alpha_q31 = FM_DEEMPH_ALPHA_300US_Q31;
            }
            else
            {
                deemph_alpha_q31 = FM_DEEMPH_ALPHA_50US_Q31;
            }

            mixed = ((int64_t)(0x7FFFFFFF - deemph_alpha_q31) * (int64_t)audio_cic_q31) +
                    ((int64_t)deemph_alpha_q31 * (int64_t)s_deemph_state);
            s_deemph_state = sat_i32(mixed >> 31);
            dac_hw_stream_fm_push_sample_isr(fm_q31_to_dac12(s_deemph_state));
        }
        else
        {
            s_has_prev = 1U;
        }

        s_i_prev = i_filt;
        s_q_prev = q_filt;
    }

    return true;
}
