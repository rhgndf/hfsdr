#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace complex_dsp {

constexpr float kPi = 3.14159265358979323846264338327950288f;
constexpr float kMinPower = 1.0e-20f;
constexpr float kDbFloor = -200.0f;
constexpr float kDbCeiling = 200.0f;

struct ToneMeasurement
{
    float power = 0.0f;
    float real = 0.0f;
    float imag = 0.0f;
};

/*
 * 10*log10(x) approximation via std::bit_cast log2 split.
 * - exponent extraction gives integer log2 for free.
 * - 4th-degree minimax polynomial for log2 on [1,2], max error is about
 *   0.0003 dB for normalized positive inputs.
 */
constexpr float fast_10log10f(float x)
{
    uint32_t bits = std::bit_cast<uint32_t>(x);
    int32_t e = static_cast<int32_t>((bits >> 23) & 0xFFU) - 127;
    float m = std::bit_cast<float>((bits & 0x007FFFFFu) | 0x3F800000u);
    float l2m = -2.51285462f + (4.07009079f + (-2.12067513f + (0.64514236f - 0.08161581f * m) * m) * m) * m;
    return (static_cast<float>(e) + l2m) * 3.01029995664f;
}

inline float power_to_db(float power)
{
    float clamped_power = std::max(power, kMinPower);
    float db = fast_10log10f(clamped_power);
    return std::clamp(db, kDbFloor, kDbCeiling);
}

inline float ratio_to_db(float numerator_power, float denominator_power)
{
    return std::clamp(power_to_db(numerator_power) - power_to_db(denominator_power),
                      kDbFloor, kDbCeiling);
}

inline ToneMeasurement measure_interleaved_iq_tone(const int32_t *iq_samples,
                                                   size_t complex_count,
                                                   float sample_rate_hz,
                                                   float tone_hz)
{
    ToneMeasurement out{};
    if((iq_samples == nullptr) || (complex_count == 0U) || (sample_rate_hz <= 0.0f))
    {
        return out;
    }

    float phase_step = -2.0f * kPi * tone_hz / sample_rate_hz;
    float c_step = std::cos(phase_step);
    float s_step = std::sin(phase_step);
    float osc_re = 1.0f;
    float osc_im = 0.0f;
    float sum_re = 0.0f;
    float sum_im = 0.0f;

    for(size_t i = 0U; i < complex_count; ++i)
    {
        float sample_re = static_cast<float>(iq_samples[2U * i]);
        float sample_im = static_cast<float>(iq_samples[(2U * i) + 1U]);

        sum_re += (sample_re * osc_re) - (sample_im * osc_im);
        sum_im += (sample_re * osc_im) + (sample_im * osc_re);

        float next_re = (osc_re * c_step) - (osc_im * s_step);
        float next_im = (osc_re * s_step) + (osc_im * c_step);
        osc_re = next_re;
        osc_im = next_im;
    }

    float inv_count = 1.0f / static_cast<float>(complex_count);
    out.real = sum_re * inv_count;
    out.imag = sum_im * inv_count;
    out.power = (out.real * out.real) + (out.imag * out.imag);
    return out;
}

} // namespace complex_dsp
