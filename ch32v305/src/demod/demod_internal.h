#pragma once

#include "demod/demod.h"

extern "C" {
#include "hw/dac.h"
}

#include <cstddef>
#include <cstdint>

namespace demod
{
constexpr uint32_t kSampleRateHz = 192000U;

uint16_t audio_to_dac12(int32_t sample, uint32_t gain_q16, uint8_t shift);
void output_reset();

void fm_reset();
void fm_process_wbfm_i2s_words(const uint16_t *words, size_t frame_count, uint32_t gain_q16);
void fm_process_nbfm_i2s_words(const uint16_t *words, size_t frame_count, uint32_t gain_q16);

void am_reset();
void am_process_i2s_words(const uint16_t *words, size_t frame_count, uint32_t gain_q16);

void ssb_reset();
void ssb_process_usb_i2s_words(const uint16_t *words, size_t frame_count, uint32_t gain_q16);
void ssb_process_lsb_i2s_words(const uint16_t *words, size_t frame_count, uint32_t gain_q16);
}
