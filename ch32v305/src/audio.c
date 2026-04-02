#include <stddef.h>
#include <stdint.h>

#include "tusb.h"
#include "usb_descriptors.h"

#define AUDIO_MIC_CTRL_CHANNELS      2U
#define AUDIO_USB_CHANNELS           2U
#define AUDIO_USB_BYTES_PER_SAMPLE   4U
#define AUDIO_USB_FRAME_BYTES        (AUDIO_USB_CHANNELS * AUDIO_USB_BYTES_PER_SAMPLE)
#define AUDIO_USB_DMA_CHUNK_FRAMES   64U

#define VOLUME_CTRL_0_DB   0
#define VOLUME_CTRL_50_DB  12800 // 50 dB in 1/256 dB units

static const uint32_t sample_rates[] = {192000U};
static uint32_t current_sample_rate = 192000U;

static int8_t mute[AUDIO_MIC_CTRL_CHANNELS + 1U];
static int16_t volume[AUDIO_MIC_CTRL_CHANNELS + 1U];
static uint8_t s_streaming_alt = 0U;
static volatile uint32_t s_usb_drop_frame_count = 0U;
static uint32_t s_usb_stream_buf[AUDIO_USB_DMA_CHUNK_FRAMES * AUDIO_USB_CHANNELS];

[[nodiscard]] bool audio_usb_tx_ready(void)
{
  return s_streaming_alt != 0U;
}

void audio_usb_mic_write_isr(volatile uint16_t const* src_words, size_t word_count)
{
  size_t const frame_count = word_count / 4U;
  size_t frame_idx;
  tu_fifo_t* fifo;

  if ((!audio_usb_tx_ready()) || (src_words == 0) || (frame_count == 0U)) {
    return;
  }

  if (frame_count > AUDIO_USB_DMA_CHUNK_FRAMES) {
    s_usb_drop_frame_count += (uint32_t)frame_count;
    return;
  }

  fifo = tud_audio_get_ep_in_ff();
  if (tu_fifo_remaining(fifo) < (frame_count * AUDIO_USB_FRAME_BYTES)) {
    s_usb_drop_frame_count += (uint32_t)frame_count;
    return;
  }

  for (frame_idx = 0U; frame_idx < frame_count; ++frame_idx) {
    uint16_t const left_hi = src_words[frame_idx * 4U];
    uint16_t const left_lo = src_words[frame_idx * 4U + 1U];
    uint16_t const right_hi = src_words[frame_idx * 4U + 2U];
    uint16_t const right_lo = src_words[frame_idx * 4U + 3U];

    s_usb_stream_buf[frame_idx * 2U] = ((uint32_t)left_hi << 16) | (uint32_t)left_lo;
    s_usb_stream_buf[frame_idx * 2U + 1U] = ((uint32_t)right_hi << 16) | (uint32_t)right_lo;
  }

  (void)tud_audio_write(s_usb_stream_buf, (uint16_t)(frame_count * AUDIO_USB_FRAME_BYTES));
}

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
  uint32_t requested_rate;
  uint8_t i;

  TU_ASSERT(request->bEntityID == UAC2_ENTITY_CLOCK);
  TU_VERIFY(request->bRequest == AUDIO20_CS_REQ_CUR);

  if (request->bControlSelector == AUDIO20_CS_CTRL_SAM_FREQ) {
    TU_VERIFY(request->wLength == sizeof(audio20_control_cur_4_t));
    requested_rate = (uint32_t) tu_le32toh(((audio20_control_cur_4_t const*) buf)->bCur);

    for (i = 0U; i < TU_ARRAY_SIZE(sample_rates); ++i) {
      if (sample_rates[i] == requested_rate) {
        current_sample_rate = requested_rate;
        return true;
      }
    }

    return false;
  }

  return false;
}

static bool audio_input_terminal_get_request(uint8_t rhport, audio20_control_request_t const* request) {
  TU_ASSERT(request->bEntityID == UAC2_ENTITY_MIC_INPUT_TERMINAL);

  if (request->bControlSelector == AUDIO20_TE_CTRL_CONNECTOR &&
      request->bRequest == AUDIO20_CS_REQ_CUR) {
    audio20_desc_channel_cluster_t connector = {
      .bNrChannels = AUDIO_USB_CHANNELS,
      .bmChannelConfig = tu_htole32(AUDIO20_CHANNEL_CONFIG_NON_PREDEFINED),
      .iChannelNames = 0U
    };

    return tud_audio_buffer_and_schedule_control_xfer(
        rhport, (tusb_control_request_t const*) request, &connector, sizeof(connector));
  }

  return false;
}

static bool audio_feature_unit_get_request(uint8_t rhport, audio20_control_request_t const* request) {
  TU_ASSERT(request->bEntityID == UAC2_ENTITY_MIC_FEATURE_UNIT);
  TU_VERIFY(request->bChannelNumber <= AUDIO_MIC_CTRL_CHANNELS);

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

  TU_ASSERT(request->bEntityID == UAC2_ENTITY_MIC_FEATURE_UNIT);
  TU_VERIFY(request->bRequest == AUDIO20_CS_REQ_CUR);
  TU_VERIFY(request->bChannelNumber <= AUDIO_MIC_CTRL_CHANNELS);

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

  if (request->bEntityID == UAC2_ENTITY_MIC_INPUT_TERMINAL) {
    return audio_input_terminal_get_request(rhport, request);
  }

  if (request->bEntityID == UAC2_ENTITY_CLOCK) {
    return audio_clock_get_request(rhport, request);
  }

  if (request->bEntityID == UAC2_ENTITY_MIC_FEATURE_UNIT) {
    return audio_feature_unit_get_request(rhport, request);
  }

  return false;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const* p_request, uint8_t* buf) {
  audio20_control_request_t const* request = (audio20_control_request_t const*) p_request;

  if (request->bEntityID == UAC2_ENTITY_CLOCK) {
    return audio_clock_set_request(rhport, request, buf);
  }

  if (request->bEntityID == UAC2_ENTITY_MIC_FEATURE_UNIT) {
    return audio_feature_unit_set_request(rhport, request, buf);
  }

  return false;
}

bool tud_audio_set_req_ep_cb(uint8_t rhport, tusb_control_request_t const* p_request, uint8_t* buf) {
  (void)rhport;
  (void)p_request;
  (void)buf;
  return false;
}

bool tud_audio_get_req_ep_cb(uint8_t rhport, tusb_control_request_t const* p_request) {
  (void)rhport;
  (void)p_request;
  return false;
}

bool tud_audio_set_req_itf_cb(uint8_t rhport, tusb_control_request_t const* p_request, uint8_t* buf) {
  (void)rhport;
  (void)p_request;
  (void)buf;
  return false;
}

bool tud_audio_get_req_itf_cb(uint8_t rhport, tusb_control_request_t const* p_request) {
  (void)rhport;
  (void)p_request;
  return false;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const* p_request) {
  (void)rhport;

  if (tu_u16_low(tu_le16toh(p_request->wIndex)) == ITF_NUM_AUDIO_STREAMING_MIC) {
    s_streaming_alt = 0U;
  }

  return true;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const* p_request) {
  (void)rhport;

  if (tu_u16_low(tu_le16toh(p_request->wIndex)) == ITF_NUM_AUDIO_STREAMING_MIC) {
    s_streaming_alt = tu_u16_low(tu_le16toh(p_request->wValue));
    tud_audio_clear_ep_in_ff();
  }

  return true;
}
