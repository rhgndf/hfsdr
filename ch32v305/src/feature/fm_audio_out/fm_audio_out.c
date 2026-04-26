#include "feature/fm_audio_out/fm_audio_out.h"

#include "hw/dac_hw.h"

/* State mirrors the fixed-point path: discriminator -> 4-sample CIC -> deemph -> DAC DMA. */
static int32_t s_i_prev = 0;
static int32_t s_q_prev = 0;
static int32_t s_deemph_state = 0;
static int32_t s_dac_sd_error_q19 = 0;
static int32_t s_cic_hist_q31[4] = {0};
static int64_t s_cic_sum_q31 = 0;
static uint8_t s_cic_idx = 0U;
static uint8_t s_has_prev = 0U;

/* pi-domain constants in Q29 so small-angle gain matches the prior num/den path. */
static const int32_t s_pi_q29 = 1686629713;
static const int32_t s_pi_over_2_q29 = 843314857;

/* alpha = exp(-1/(192000*50e-6)) in Q31 */
static const int32_t s_deemph_alpha_q31 = 1935044054;
static const int32_t s_one_minus_alpha_q31 = (int32_t)(0x7FFFFFFF - 1935044054);

/*
 * Odd 5th-degree least-squares fit of atan(x) on [0, 1]:
 * atan(x) ~= x * (c1 + x^2 * (c3 + x^2 * c5))
 * Coefficients are stored in Q31 and evaluated with Horner form.
 */
static const int32_t s_atan_c1_q31 = 2138856262;
static const int32_t s_atan_c3_q31 = -627668775;
static const int32_t s_atan_c5_q31 = 178286847;

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

static int32_t cic4_q31(int32_t x_q31)
{
    int32_t old_q31 = s_cic_hist_q31[s_cic_idx];

    s_cic_hist_q31[s_cic_idx] = x_q31;
    s_cic_idx = (s_cic_idx + 1U) & 3U;
    s_cic_sum_q31 += (int64_t)x_q31 - (int64_t)old_q31;

    return sat_i32(s_cic_sum_q31 >> 2);
}

static uint64_t abs_i64(int64_t x)
{
    return (x < 0) ? (uint64_t)(-x) : (uint64_t)x;
}

static int32_t atan_0_1_q29(uint32_t x_q31)
{
    uint64_t x2_q31 = ((uint64_t)x_q31 * (uint64_t)x_q31) >> 31;
    int64_t acc_q31 = s_atan_c5_q31;

    acc_q31 = (int64_t)s_atan_c3_q31 + ((acc_q31 * (int64_t)x2_q31) >> 31);
    acc_q31 = (int64_t)s_atan_c1_q31 + ((acc_q31 * (int64_t)x2_q31) >> 31);

    return (int32_t)((((int64_t)x_q31 * acc_q31) >> 31) >> 2);
}

static int32_t atan2_q29(int64_t y, int64_t x)
{
    uint64_t ay = abs_i64(y);
    uint64_t ax = abs_i64(x);
    int32_t base_q29;
    uint32_t ratio_q31;

    if((ax == 0U) && (ay == 0U))
    {
        return 0;
    }

    if(ax >= ay)
    {
        ratio_q31 = (ax == 0U) ? 0U : (uint32_t)((ay << 31) / ax);
        base_q29 = atan_0_1_q29(ratio_q31);
    }
    else
    {
        ratio_q31 = (uint32_t)((ax << 31) / ay);
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
    int64_t shaped_q19 = ((int64_t)2048 << 19) + ((int64_t)y_q31 >> 1) + (int64_t)s_dac_sd_error_q19;
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

void fm_audio_out_init(void)
{
    s_i_prev = 0;
    s_q_prev = 0;
    s_deemph_state = 0;
    s_dac_sd_error_q19 = 0;
    s_cic_hist_q31[0] = 0;
    s_cic_hist_q31[1] = 0;
    s_cic_hist_q31[2] = 0;
    s_cic_hist_q31[3] = 0;
    s_cic_sum_q31 = 0;
    s_cic_idx = 0U;
    s_has_prev = 0U;
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
    for(i = 0U; i < frame_count; ++i)
    {
        uint16_t i_hi = src_words[i * 4U + 0U];
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
            int32_t cic_q31;
            int64_t mixed;

            fm_q31 = -atan2_q29(num, den);
            cic_q31 = cic4_q31(fm_q31);
            mixed = ((int64_t)s_one_minus_alpha_q31 * (int64_t)cic_q31) +
                    ((int64_t)s_deemph_alpha_q31 * (int64_t)s_deemph_state);
            s_deemph_state = sat_i32(mixed >> 31);
            dac_hw_stream_fm_push_sample_isr(fm_q31_to_dac12(s_deemph_state));
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
