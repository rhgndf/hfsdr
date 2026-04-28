#include "feature/fm_audio_out/fm_audio_out.h"

extern "C" {
#include "hw/dac.h"
}

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <tuple>
#include <utility>

constexpr uint32_t FM_AUDIO_OUT_GAIN_DEFAULT_Q16 = 0UL;
/* alpha = exp(-1/(Fs*tau)) in Q31 at the post-upsample audio rate of 192 kHz. */
constexpr int32_t FM_DEEMPH_ALPHA_50US_Q31  = 1935044054;  /* tau=50us,  alpha=0.901080 */
constexpr int32_t FM_DEEMPH_ALPHA_300US_Q31 = 2100413000;  /* tau=300us, alpha=0.978078 */
/* Single-pole pilot-rejection LPF applied between the audio CIC and the
 * deemph stage. alpha = exp(-2*pi*fc/fs) at fs=192kHz, fc=12kHz. Combined with
 * the single N=4 CIC and 50us deemph, the 19 kHz pilot lands at -23 dB while
 * pre-emphasis-compensated audio rolls off to -2.8 dB at 10 kHz. */
constexpr int32_t FM_PILOT_LPF_ALPHA_Q31    = 1450049478;  /* fc=12kHz,  alpha=0.675232 */
constexpr uint8_t FM_IQ_CIC_MAX_TAPS = 16U;
constexpr uint8_t FM_AUDIO_CIC_TAPS = 4U;
constexpr uint8_t FM_DAC_UPSAMPLE = 1U;

template<typename T> struct WiderInt;
template<> struct WiderInt<int8_t>   { using type = int16_t;  };
template<> struct WiderInt<int16_t>  { using type = int32_t;  };
template<> struct WiderInt<int32_t>  { using type = int64_t;  };
template<> struct WiderInt<uint8_t>  { using type = uint16_t; };
template<> struct WiderInt<uint16_t> { using type = uint32_t; };
template<> struct WiderInt<uint32_t> { using type = uint64_t; };
template<typename T> using WiderInt_t = typename WiderInt<T>::type;

template<typename T, size_t N>
class CICFilter
{
    static_assert(N > 0 && (N & (N - 1)) == 0, "N must be a power of two");
    static constexpr size_t kShift = std::countr_zero(N);
    static constexpr size_t kMask = N - 1U;

    using Sum = WiderInt_t<T>;

    T hist[N] = {0};
    Sum sum = 0;
    size_t idx = 0U;

public:
    T push(T x)
    {
        T old = hist[idx];
        hist[idx] = x;
        idx = (idx + 1U) & kMask;
        sum += Sum(x) - old;
        return T(sum >> kShift);
    }

    void reset()
    {
        std::fill(hist, hist + N, 0);
        sum = 0;
        idx = 0U;
    }
};

template<typename T, size_t N>
class CICComplexFilter
{
    static_assert(N > 0 && (N & (N - 1)) == 0, "N must be a power of two");
    static constexpr size_t kShift = std::countr_zero(N);
    static constexpr size_t kMask = N - 1U;

    using Sum = WiderInt_t<T>;

    /* Interleaved [i0,q0,i1,q1,...] so each tap fits in one cache line and the index/mask is shared. */
    T hist[2 * N] = {0};
    Sum sum_i = 0;
    Sum sum_q = 0;
    size_t idx = 0U;

public:
    std::pair<T, T> push(T i_now, T q_now)
    {
        size_t base = idx * 2U;
        T old_i = hist[base];
        T old_q = hist[base + 1U];
        hist[base] = i_now;
        hist[base + 1U] = q_now;
        idx = (idx + 1U) & kMask;
        sum_i += Sum(i_now) - old_i;
        sum_q += Sum(q_now) - old_q;
        return {T(sum_i >> kShift), T(sum_q >> kShift)};
    }

    void reset()
    {
        std::fill(hist, hist + 2 * N, 0);
        sum_i = 0;
        sum_q = 0;
        idx = 0U;
    }
};

/*
 * Single-pole IIR: y[n] = (1 - alpha) * x[n] + alpha * y[n-1], coefficients in QFRAC.
 * "One" saturates at type max for QFRAC == bit-width-1 since 2^(N-1) is unrepresentable in signed T.
 *
 * No saturating clamp on the output: with kOne == INT32_MAX (QFRAC=31), the
 * IIR is a strict convex combination — |state| <= max(|x|, |state_prev|), so
 * an input bounded by INT32_MAX keeps state bounded too. The atan2 output
 * feeding the FM path is in Q29 (< pi * 2^29 < 2^31), well within range.
 */
template<typename T, size_t QFRAC>
class SinglePoleIIR
{
    static_assert(QFRAC < sizeof(T) * 8, "QFRAC must be < bit-width of T");
    using Sum = WiderInt_t<T>;
    static constexpr T kOne = (QFRAC == sizeof(T) * 8 - 1)
                                ? std::numeric_limits<T>::max()
                                : T(T(1) << QFRAC);

    T state = 0;

public:
    T push(T x, T alpha)
    {
        Sum mixed = Sum(kOne - alpha) * x + Sum(alpha) * state;
        state = T(mixed >> QFRAC);
        return state;
    }

    T value() const { return state; }
    void reset() { state = 0; }
};

/* State mirrors the fixed-point path: optional IQ CIC -> discriminator -> audio CIC -> deemph -> DAC DMA. */
static int32_t s_i_prev = 0;
static int32_t s_q_prev = 0;
static int32_t s_dac_sd_error_q19 = 0;
static volatile uint32_t s_audio_gain_q16 = FM_AUDIO_OUT_GAIN_DEFAULT_Q16;
static CICFilter<int32_t, FM_AUDIO_CIC_TAPS> s_audio_cic_chain;
static CICComplexFilter<int32_t, FM_IQ_CIC_MAX_TAPS> s_iq_cic;
static SinglePoleIIR<int32_t, 31> s_pilot_lpf;
static SinglePoleIIR<int32_t, 31> s_deemph;
static uint8_t s_has_prev = 0U;
static volatile fm_audio_out_mode_t s_mode = FM_AUDIO_OUT_MODE_WBFM;

/* pi-domain constants in Q29 so small-angle gain matches the prior num/den path. */
static constexpr int32_t s_pi_q29 = static_cast<int32_t>(std::numbers::pi_v<double> * (1LL << 29) + 0.5);
static constexpr int32_t s_pi_over_2_q29 = static_cast<int32_t>(std::numbers::pi_v<double> / 2.0 * (1LL << 29) + 0.5);

/*
 * Odd 5th-degree minimax (Remez) fit of atan(x) on [0, 1]:
 * atan(x) ~= x * (c1 + x^2 * (c3 + x^2 * c5))
 * Max abs error ~6.1e-4 in atan (vs 1.3e-3 for the previous least-squares fit).
 * Coefficients are stored in Q31 and evaluated with Horner form.
 */
static constexpr int32_t s_atan_c1_q31 = 2137514932;
static constexpr int32_t s_atan_c3_q31 = -619957566;
static constexpr int32_t s_atan_c5_q31 = 170379294;

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
    uint32_t ay = static_cast<uint32_t>(std::abs(static_cast<int64_t>(y)));
    uint32_t ax = static_cast<uint32_t>(std::abs(static_cast<int64_t>(x)));
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
    s_audio_cic_chain.reset();
    s_iq_cic.reset();
    s_pilot_lpf.reset();
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
    s_has_prev = 0U;
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

    for(size_t i = 0U; i < frame_count; ++i)
    {
        uint16_t i_hi = words[i * 4U + 0U];
        uint16_t i_lo = words[i * 4U + 1U];
        uint16_t q_hi = words[i * 4U + 2U];
        uint16_t q_lo = words[i * 4U + 3U];
        int32_t i_now = (int32_t)(((uint32_t)i_hi << 16) | (uint32_t)i_lo);
        int32_t q_now = (int32_t)(((uint32_t)q_hi << 16) | (uint32_t)q_lo);

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

        if(s_has_prev != 0U)
        {
            /* mulh-truncate each product before combining: 4 mulh + add/sub vs full 64-bit subtract with borrow.
               on very rare cases when inputs are at the limits, num/den may overflow, but practically that won't happen
             */
            int32_t num = (int32_t)(((int64_t)i_filt * (int64_t)s_q_prev) >> 32)
                        - (int32_t)(((int64_t)q_filt * (int64_t)s_i_prev) >> 32);
            int32_t den = (int32_t)(((int64_t)i_filt * (int64_t)s_i_prev) >> 32)
                        + (int32_t)(((int64_t)q_filt * (int64_t)s_q_prev) >> 32);

            int32_t fm_q31 = -atan2_q29(num, den);
            /* No upsampling at 192 kHz; the discriminator output is filtered by
             * a single length-4 CIC (null at 48 kHz, ~-2.2 dB at 19 kHz)
             * followed by a 1-pole IIR at fc=12 kHz to attenuate the 19 kHz
             * pilot before deemph compensates. */
            int32_t audio_cic_q31 = s_audio_cic_chain.push(fm_q31);
            int32_t audio_lpf_q31 = s_pilot_lpf.push(audio_cic_q31, FM_PILOT_LPF_ALPHA_Q31);
            dac_hw_stream_fm_push_sample_isr(fm_q31_to_dac12<MODE>(s_deemph.push(audio_lpf_q31, deemph_alpha_q31), gain_q16));
        }
        else
        {
            s_has_prev = 1U;
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
