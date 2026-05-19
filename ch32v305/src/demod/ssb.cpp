#include "demod/demod_internal.h"

#include "utils/dsp.h"

#include <array>
#include <cmath>
#include <numbers>

namespace
{
constexpr uint8_t kCoeffQ = 30U;
constexpr uint8_t kInputShift = 6U;
constexpr uint8_t kDacShift = 5U;
constexpr size_t kOscillatorPeriod = 128U;
constexpr size_t kOscillatorMask = kOscillatorPeriod - 1U;

class WeaverLowpass
{
    /*
     * Elliptic low-pass, Fs=192 kHz, passband 0-1.2 kHz, stopband from 1.8 kHz,
     * 0.5 dB passband ripple, 50 dB stopband attenuation. Coefficients are SOS
     * form quantized to Q30.
     */
    static constexpr BiquadCoefficients<kCoeffQ> kSection0 = {
        419378, 419378, 0, -1055847830, 0
    };
    static constexpr BiquadCoefficients<kCoeffQ> kSection1 = {
        1073741824, -2138720392, 1073741824, -2122788773, 1049994885
    };
    static constexpr BiquadCoefficients<kCoeffQ> kSection2 = {
        1073741824, -2143554055, 1073741824, -2138975743, 1066936715
    };

    BiquadState<kCoeffQ> section0;
    BiquadState<kCoeffQ> section1;
    BiquadState<kCoeffQ> section2;

public:
    int32_t push(int32_t x)
    {
        return section2.push(section1.push(section0.push(x, kSection0), kSection1), kSection2);
    }

    void reset()
    {
        section0.reset();
        section1.reset();
        section2.reset();
    }
};

struct OscillatorSample
{
    int32_t cos_q30;
    int32_t sin_q30;
};

consteval int32_t unit_to_q30(double value)
{
    double scaled = value * static_cast<double>(1LL << kCoeffQ);
    return static_cast<int32_t>(scaled >= 0.0 ? scaled + 0.5 : scaled - 0.5);
}

consteval std::array<OscillatorSample, kOscillatorPeriod> make_mixer_table()
{
    std::array<OscillatorSample, kOscillatorPeriod> table{};
    for(size_t i = 0U; i < table.size(); ++i)
    {
        double phase = 2.0 * std::numbers::pi_v<double> *
                       static_cast<double>(i) / static_cast<double>(kOscillatorPeriod);
        table[i] = {
            unit_to_q30(std::cos(phase)),
            unit_to_q30(std::sin(phase)),
        };
    }
    return table;
}

static constexpr auto kMixerTable = make_mixer_table();

static WeaverLowpass s_i_lpf;
static WeaverLowpass s_q_lpf;
static size_t s_mixer_idx = 0U;

static int32_t q30_mul(int32_t a, int32_t b)
{
    return (int32_t)(((int64_t)a * (int64_t)b) >> kCoeffQ);
}

template<demodulation_mode_t MODE>
static void ssb_process_frames(const uint16_t *words, size_t frame_count, uint32_t gain_q16)
{
    static_assert((MODE == DEMODULATION_MODE_USB) || (MODE == DEMODULATION_MODE_LSB));

    uint32_t const *words32 = (uint32_t const *)(uintptr_t)words;
    size_t mixer_idx = s_mixer_idx;

    for(size_t i = 0U; i < frame_count; ++i)
    {
        uint32_t raw_i = words32[i * 2U];
        uint32_t raw_q = words32[i * 2U + 1U];
        int32_t i_now = (int32_t)((raw_i << 16) | (raw_i >> 16)) >> kInputShift;
        int32_t q_now = (int32_t)((raw_q << 16) | (raw_q >> 16)) >> kInputShift;
        if constexpr(MODE == DEMODULATION_MODE_LSB)
        {
            q_now = -q_now;
        }

        const auto [cos_q30, sin_q30] = kMixerTable[mixer_idx];

        int32_t mixed_i = q30_mul(i_now, cos_q30) + q30_mul(q_now, sin_q30);
        int32_t mixed_q = q30_mul(q_now, cos_q30) - q30_mul(i_now, sin_q30);
        int32_t filt_i = s_i_lpf.push(mixed_i);
        int32_t filt_q = s_q_lpf.push(mixed_q);
        int32_t audio = q30_mul(filt_i, cos_q30) - q30_mul(filt_q, sin_q30);

        uint16_t const dac12 = demod::audio_to_dac12(audio, gain_q16, kDacShift);
        dac_hw_stream_fm_push_sample_isr(dac12);

        mixer_idx = (mixer_idx + 1U) & kOscillatorMask;
    }

    s_mixer_idx = mixer_idx;
}
}

namespace demod
{
void ssb_reset()
{
    s_i_lpf.reset();
    s_q_lpf.reset();
    s_mixer_idx = 0U;
}

void ssb_process_usb_i2s_words(const uint16_t *words, size_t frame_count, uint32_t gain_q16)
{
    ssb_process_frames<DEMODULATION_MODE_USB>(words, frame_count, gain_q16);
}

void ssb_process_lsb_i2s_words(const uint16_t *words, size_t frame_count, uint32_t gain_q16)
{
    ssb_process_frames<DEMODULATION_MODE_LSB>(words, frame_count, gain_q16);
}
}
