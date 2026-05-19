#include "demod/demod_internal.h"

#include "utils/dsp.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr uint8_t kBiquadQ = 30U;
constexpr uint8_t kInputShift = 3U;
constexpr uint8_t kEnvelopeShift = 8U;
constexpr uint8_t kNormQ = 20U;
constexpr uint8_t kNormGainQ = 30U;
constexpr uint8_t kNormGainShift = kNormGainQ - kNormQ;
constexpr uint8_t kDacShift = 3U;
constexpr uint8_t kAudioCicTaps = 4U;
constexpr int32_t kCarrierLpfAlphaQ31 = 2146435071; /* 1 - 1/2048 in Q31 */
constexpr int32_t kMinCarrier = 1 << 10;
constexpr int32_t kMaxNormalizedAudio = 1 << 21;

class Cheby2Lowpass
{
    static constexpr BiquadCoefficients<kBiquadQ> kSection0 = {
        10517004, -14428567, 10517004, -1811780164, 769196170
    };
    static constexpr BiquadCoefficients<kBiquadQ> kSection1 = {
        1073741824, -2014475559, 1073741824, -2005123829, 959579643
    };

    BiquadState<kBiquadQ> section0;
    BiquadState<kBiquadQ> section1;

public:
    int32_t push(int32_t x)
    {
        return section1.push(section0.push(x, kSection0), kSection1);
    }

    void reset()
    {
        section0.reset();
        section1.reset();
    }
};

static Cheby2Lowpass s_i_lpf;
static Cheby2Lowpass s_q_lpf;
static CICFilter<int32_t, kAudioCicTaps> s_audio_cic;
static SinglePoleIIR<int32_t, 31> s_carrier_lpf;
static uint32_t s_norm_gain_q30 = 0;

static int32_t exact_envelope(int32_t i_filt, int32_t q_filt)
{
    float fi = static_cast<float>(i_filt >> kEnvelopeShift);
    float fq = static_cast<float>(q_filt >> kEnvelopeShift);
    float mag = __builtin_sqrtf((fi * fi) + (fq * fq));
    return static_cast<int32_t>(mag);
}

static uint32_t carrier_norm_gain_q30(int32_t carrier)
{
    uint32_t clamped_carrier = static_cast<uint32_t>(carrier < kMinCarrier ? kMinCarrier : carrier);
    return static_cast<uint32_t>(((uint64_t)1 << kNormGainQ) / clamped_carrier);
}

static int32_t normalize_modulation_depth(int32_t audio, uint32_t norm_gain_q30)
{
    int64_t normalized = ((int64_t)audio * (int64_t)norm_gain_q30) >> kNormGainShift;
    return static_cast<int32_t>(std::clamp<int64_t>(normalized,
                                                    -kMaxNormalizedAudio,
                                                    kMaxNormalizedAudio));
}
}

namespace demod
{
void am_reset()
{
    s_i_lpf.reset();
    s_q_lpf.reset();
    s_audio_cic.reset();
    s_carrier_lpf.seed(kMinCarrier);
    s_norm_gain_q30 = carrier_norm_gain_q30(kMinCarrier);
}

void am_process_i2s_words(const uint16_t *words, size_t frame_count, uint32_t gain_q16)
{
    uint32_t const *words32 = (uint32_t const *)(uintptr_t)words;
    int32_t carrier = s_carrier_lpf.value();
    uint32_t norm_gain_q30 = s_norm_gain_q30;

    for(size_t i = 0U; i < frame_count; ++i)
    {
        uint32_t raw_i = words32[i * 2U];
        uint32_t raw_q = words32[i * 2U + 1U];
        int32_t i_now = (int32_t)((raw_i << 16) | (raw_i >> 16));
        int32_t q_now = (int32_t)((raw_q << 16) | (raw_q >> 16));
        int32_t i_filt = s_i_lpf.push(i_now >> kInputShift);
        int32_t q_filt = s_q_lpf.push(q_now >> kInputShift);

        int32_t env = exact_envelope(i_filt, q_filt);
        carrier = s_carrier_lpf.push(env, kCarrierLpfAlphaQ31);
        int32_t normalized_audio = normalize_modulation_depth(env - carrier, norm_gain_q30);
        int32_t audio = s_audio_cic.push(normalized_audio);

        uint16_t const dac12 = demod::audio_to_dac12(audio, gain_q16, kDacShift);
        dac_hw_stream_fm_push_sample_isr(dac12);
    }

    s_norm_gain_q30 = carrier_norm_gain_q30(carrier);
}
}
