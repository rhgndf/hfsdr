#include "demod/demod.h"

#include "demod/demod_internal.h"

#include <algorithm>

namespace
{
constexpr uint32_t kDemodGainDefaultQ16 = 0UL;

static volatile uint32_t s_audio_gain_q16 = kDemodGainDefaultQ16;
static volatile demodulation_mode_t s_mode = DEMODULATION_MODE_WBFM;
static int32_t s_dac_sd_error_q19 = 0;
}

namespace demod
{
uint16_t audio_to_dac12(int32_t sample, uint32_t gain_q16, uint8_t shift)
{
    int64_t audio_q19 = ((int64_t)sample * (int64_t)gain_q16) >> shift;
    int64_t shaped_q19 = ((int64_t)2048 << 19) + audio_q19 + (int64_t)s_dac_sd_error_q19;
    int64_t dac64 = (shaped_q19 + ((int64_t)1 << 18)) >> 19;
    int64_t clamped64 = std::clamp<int64_t>(dac64, (int64_t)0, (int64_t)4095);
    int32_t residual = (int32_t)(shaped_q19 - (clamped64 << 19));
    int32_t not_sat_mask = -(int32_t)(clamped64 == dac64);
    s_dac_sd_error_q19 = residual & not_sat_mask;
    return (uint16_t)clamped64;
}

void output_reset()
{
    s_dac_sd_error_q19 = 0;
}
}

static void demod_reset_filters()
{
    demod::output_reset();
    demod::fm_reset();
    demod::am_reset();
    demod::ssb_reset();
}

extern "C" void demod_set_gain(uint32_t gain_q16)
{
    s_audio_gain_q16 = gain_q16;
}

extern "C" void demod_set_mode(demodulation_mode_t mode)
{
    if(mode >= DEMODULATION_MODE_COUNT)
    {
        mode = DEMODULATION_MODE_WBFM;
    }

    if(mode == s_mode)
    {
        return;
    }

    s_mode = mode;
    demod_reset_filters();
}

extern "C" demodulation_mode_t demod_get_mode(void)
{
    return s_mode;
}

extern "C" void demod_init(void)
{
    s_audio_gain_q16 = kDemodGainDefaultQ16;
    s_mode = DEMODULATION_MODE_WBFM;
    demod_reset_filters();
    dac_hw_stream_fm_start(demod::kSampleRateHz);
}

extern "C" bool demod_process_isr(volatile uint16_t const *src_words, size_t word_count)
{
    if((src_words == nullptr) || (word_count < 4U))
    {
        return false;
    }

    size_t frame_count = word_count / 4U;
    const uint16_t *words = const_cast<const uint16_t *>(src_words);
    uint32_t const gain_q16 = s_audio_gain_q16;

    switch(s_mode)
    {
        case DEMODULATION_MODE_NBFM:
            demod::fm_process_nbfm_i2s_words(words, frame_count, gain_q16);
            break;

        case DEMODULATION_MODE_AM:
            demod::am_process_i2s_words(words, frame_count, gain_q16);
            break;

        case DEMODULATION_MODE_USB:
            demod::ssb_process_usb_i2s_words(words, frame_count, gain_q16);
            break;

        case DEMODULATION_MODE_LSB:
            demod::ssb_process_lsb_i2s_words(words, frame_count, gain_q16);
            break;

        case DEMODULATION_MODE_WBFM:
        default:
            demod::fm_process_wbfm_i2s_words(words, frame_count, gain_q16);
            break;
    }

    return true;
}
