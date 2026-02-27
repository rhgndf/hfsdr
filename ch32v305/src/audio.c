#include <stdint.h>

#include "tusb.h"
#include "usb_descriptors.h"

// Descriptor advertises master + 2 speaker channels in the feature unit.
#define AUDIO_SPK_CTRL_CHANNELS 2

#define VOLUME_CTRL_0_DB   0
#define VOLUME_CTRL_50_DB  12800 // 50 dB in 1/256 dB units

static const uint32_t sample_rates[] = {44100, 48000, 96000};
static uint32_t current_sample_rate = 48000;

static int8_t mute[AUDIO_SPK_CTRL_CHANNELS + 1];
static int16_t volume[AUDIO_SPK_CTRL_CHANNELS + 1];

static bool audio_clock_get_request(uint8_t rhport, audio20_control_request_t const* request) {
  TU_ASSERT(request->bEntityID == UAC2_ENTITY_CLOCK);

  if (request->bControlSelector == AUDIO20_CS_CTRL_SAM_FREQ) {
    if (request->bRequest == AUDIO20_CS_REQ_CUR) {
      audio20_control_cur_4_t curf = { (int32_t) tu_htole32(current_sample_rate) };
      return tud_audio_buffer_and_schedule_control_xfer(
          rhport, (tusb_control_request_t const*) request, &curf, sizeof(curf));
    } else if (request->bRequest == AUDIO20_CS_REQ_RANGE) {
      enum { N_SAMPLE_RATES = TU_ARRAY_SIZE(sample_rates) };
      audio20_control_range_4_n_t(N_SAMPLE_RATES) rangef = {
        .wNumSubRanges = tu_htole16(N_SAMPLE_RATES)
      };

      for (uint8_t i = 0; i < N_SAMPLE_RATES; i++) {
        rangef.subrange[i].bMin = (int32_t) tu_htole32(sample_rates[i]);
        rangef.subrange[i].bMax = (int32_t) tu_htole32(sample_rates[i]);
        rangef.subrange[i].bRes = 0;
      }

      return tud_audio_buffer_and_schedule_control_xfer(
          rhport, (tusb_control_request_t const*) request, &rangef, sizeof(rangef));
    }
  } else if (request->bControlSelector == AUDIO20_CS_CTRL_CLK_VALID &&
             request->bRequest == AUDIO20_CS_REQ_CUR) {
    audio20_control_cur_1_t cur_valid = { .bCur = 1 };
    return tud_audio_buffer_and_schedule_control_xfer(
        rhport, (tusb_control_request_t const*) request, &cur_valid, sizeof(cur_valid));
  }

  return false;
}

static bool audio_clock_set_request(uint8_t rhport, audio20_control_request_t const* request, uint8_t const* buf) {
  (void) rhport;

  TU_ASSERT(request->bEntityID == UAC2_ENTITY_CLOCK);
  TU_VERIFY(request->bRequest == AUDIO20_CS_REQ_CUR);

  if (request->bControlSelector == AUDIO20_CS_CTRL_SAM_FREQ) {
    TU_VERIFY(request->wLength == sizeof(audio20_control_cur_4_t));
    current_sample_rate = (uint32_t) tu_le32toh(((audio20_control_cur_4_t const*) buf)->bCur);
    return true;
  }

  return false;
}

static bool audio_feature_unit_get_request(uint8_t rhport, audio20_control_request_t const* request) {
  TU_ASSERT(request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT);
  TU_VERIFY(request->bChannelNumber <= AUDIO_SPK_CTRL_CHANNELS);

  if (request->bControlSelector == AUDIO20_FU_CTRL_MUTE &&
      request->bRequest == AUDIO20_CS_REQ_CUR) {
    audio20_control_cur_1_t cur_mute = { .bCur = mute[request->bChannelNumber] };
    return tud_audio_buffer_and_schedule_control_xfer(
        rhport, (tusb_control_request_t const*) request, &cur_mute, sizeof(cur_mute));
  }

  if (request->bControlSelector == AUDIO20_FU_CTRL_VOLUME) {
    if (request->bRequest == AUDIO20_CS_REQ_RANGE) {
      audio20_control_range_2_n_t(1) range_vol = {
        .wNumSubRanges = tu_htole16(1),
        .subrange = {
          {
            .bMin = tu_htole16(-VOLUME_CTRL_50_DB),
            .bMax = tu_htole16(VOLUME_CTRL_0_DB),
            .bRes = tu_htole16(256)
          }
        }
      };
      return tud_audio_buffer_and_schedule_control_xfer(
          rhport, (tusb_control_request_t const*) request, &range_vol, sizeof(range_vol));
    }

    if (request->bRequest == AUDIO20_CS_REQ_CUR) {
      audio20_control_cur_2_t cur_vol = { .bCur = tu_htole16(volume[request->bChannelNumber]) };
      return tud_audio_buffer_and_schedule_control_xfer(
          rhport, (tusb_control_request_t const*) request, &cur_vol, sizeof(cur_vol));
    }
  }

  return false;
}

static bool audio_feature_unit_set_request(uint8_t rhport, audio20_control_request_t const* request, uint8_t const* buf) {
  (void) rhport;

  TU_ASSERT(request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT);
  TU_VERIFY(request->bRequest == AUDIO20_CS_REQ_CUR);
  TU_VERIFY(request->bChannelNumber <= AUDIO_SPK_CTRL_CHANNELS);

  if (request->bControlSelector == AUDIO20_FU_CTRL_MUTE) {
    TU_VERIFY(request->wLength == sizeof(audio20_control_cur_1_t));
    mute[request->bChannelNumber] = ((audio20_control_cur_1_t const*) buf)->bCur;
    return true;
  }

  if (request->bControlSelector == AUDIO20_FU_CTRL_VOLUME) {
    TU_VERIFY(request->wLength == sizeof(audio20_control_cur_2_t));
    volume[request->bChannelNumber] =
        (int16_t) tu_le16toh(((audio20_control_cur_2_t const*) buf)->bCur);
    return true;
  }

  return false;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const* p_request) {
  audio20_control_request_t const* request = (audio20_control_request_t const*) p_request;

  if (request->bEntityID == UAC2_ENTITY_CLOCK) {
    return audio_clock_get_request(rhport, request);
  }

  if (request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT) {
    return audio_feature_unit_get_request(rhport, request);
  }

  return false;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const* p_request, uint8_t* buf) {
  audio20_control_request_t const* request = (audio20_control_request_t const*) p_request;

  if (request->bEntityID == UAC2_ENTITY_CLOCK) {
    return audio_clock_set_request(rhport, request, buf);
  }

  if (request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT) {
    return audio_feature_unit_set_request(rhport, request, buf);
  }

  return false;
}
