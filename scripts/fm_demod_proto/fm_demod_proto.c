#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS_IQ 192000
#define FS_AUDIO 48000
#define DECIM (FS_IQ / FS_AUDIO)

/* Saturating helpers keep fixed-point math bounded. */
static int32_t sat_i32(int64_t x) {
  if (x > INT32_MAX) return INT32_MAX;
  if (x < INT32_MIN) return INT32_MIN;
  return (int32_t)x;
}

static int16_t sat_i16(int32_t x) {
  if (x > INT16_MAX) return INT16_MAX;
  if (x < INT16_MIN) return INT16_MIN;
  return (int16_t)x;
}

static uint32_t read_u32_le(FILE *f, int *ok) {
  uint8_t b[4];
  if (fread(b, 1, 4, f) != 4) {
    *ok = 0;
    return 0U;
  }
  *ok = 1;
  return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/* Minimal PCM WAV header writer (mono, 16-bit). */
static void write_wav_header(FILE *f, uint32_t sample_rate, uint16_t channels, uint16_t bits_per_sample, uint32_t data_bytes) {
  uint32_t byte_rate = sample_rate * channels * (bits_per_sample / 8);
  uint16_t block_align = channels * (bits_per_sample / 8);
  uint32_t riff_size = 36U + data_bytes;
  uint8_t h[44];
  memset(h, 0, sizeof(h));

  memcpy(&h[0], "RIFF", 4);
  h[4] = (uint8_t)(riff_size & 0xFF);
  h[5] = (uint8_t)((riff_size >> 8) & 0xFF);
  h[6] = (uint8_t)((riff_size >> 16) & 0xFF);
  h[7] = (uint8_t)((riff_size >> 24) & 0xFF);
  memcpy(&h[8], "WAVE", 4);
  memcpy(&h[12], "fmt ", 4);
  h[16] = 16;
  h[20] = 1;  /* PCM */
  h[22] = (uint8_t)(channels & 0xFF);
  h[23] = (uint8_t)((channels >> 8) & 0xFF);
  h[24] = (uint8_t)(sample_rate & 0xFF);
  h[25] = (uint8_t)((sample_rate >> 8) & 0xFF);
  h[26] = (uint8_t)((sample_rate >> 16) & 0xFF);
  h[27] = (uint8_t)((sample_rate >> 24) & 0xFF);
  h[28] = (uint8_t)(byte_rate & 0xFF);
  h[29] = (uint8_t)((byte_rate >> 8) & 0xFF);
  h[30] = (uint8_t)((byte_rate >> 16) & 0xFF);
  h[31] = (uint8_t)((byte_rate >> 24) & 0xFF);
  h[32] = (uint8_t)(block_align & 0xFF);
  h[33] = (uint8_t)((block_align >> 8) & 0xFF);
  h[34] = (uint8_t)(bits_per_sample & 0xFF);
  h[35] = (uint8_t)((bits_per_sample >> 8) & 0xFF);
  memcpy(&h[36], "data", 4);
  h[40] = (uint8_t)(data_bytes & 0xFF);
  h[41] = (uint8_t)((data_bytes >> 8) & 0xFF);
  h[42] = (uint8_t)((data_bytes >> 16) & 0xFF);
  h[43] = (uint8_t)((data_bytes >> 24) & 0xFF);

  fwrite(h, 1, sizeof(h), f);
}

int main(int argc, char **argv) {
  const char *in_path = NULL;
  const char *out_wav_path = NULL;
  const char *report_path = NULL;

  FILE *fin = NULL;
  FILE *fout = NULL;
  FILE *frep = NULL;

  int32_t i_prev = 0, q_prev = 0;
  int32_t audio_lp = 0;
  int32_t deemph_state = 0;
  int64_t peak_abs = 0;
  uint64_t count_out = 0;
  uint64_t sat_count = 0;
  uint64_t den_guard_count = 0;
  uint64_t in_pairs = 0;
  int decim_ctr = 0;

  /* alpha = exp(-1/(48000*50e-6)) in Q31 */
  const int32_t deemph_alpha_q31 = 1412758477;
  const int32_t one_minus_alpha_q31 = (int32_t)(0x7FFFFFFF - deemph_alpha_q31);

  /* Parse CLI: input IQ (i32 interleaved), output WAV, optional report. */
  if (argc < 3 || argc > 4) {
    fprintf(stderr, "Usage: %s <input_iq_i32le.bin> <output.wav> [report.txt]\n", argv[0]);
    return 2;
  }
  in_path = argv[1];
  out_wav_path = argv[2];
  if (argc == 4) {
    report_path = argv[3];
  }

  fin = fopen(in_path, "rb");
  if (!fin) {
    fprintf(stderr, "Failed to open input: %s\n", in_path);
    return 1;
  }

  fout = fopen(out_wav_path, "wb");
  if (!fout) {
    fclose(fin);
    fprintf(stderr, "Failed to open output wav: %s\n", out_wav_path);
    return 1;
  }

  if (report_path) {
    frep = fopen(report_path, "w");
  }

  /* Write placeholder WAV header, patch final sizes after streaming samples. */
  write_wav_header(fout, FS_AUDIO, 1, 16, 0);

  /*
   * Main streaming loop:
   * 1) read interleaved I/Q int32 samples
   * 2) FM discriminator (cross/dot)
   * 3) light LPF
   * 4) decimate 192k -> 48k
   * 5) 50 us de-emphasis
   * 6) write PCM16 sample
   */
  while (1) {
    int ok_i = 0, ok_q = 0;
    int32_t i_now, q_now;
    int64_t num, den;
    int32_t fm_q31 = 0;
    uint32_t raw_i = read_u32_le(fin, &ok_i);
    if (!ok_i) break;
    uint32_t raw_q = read_u32_le(fin, &ok_q);
    if (!ok_q) break;

    i_now = (int32_t)raw_i;
    q_now = (int32_t)raw_q;
    in_pairs++;

    if (in_pairs > 1) {
      /* Phase-difference discriminator terms (fixed-point cross/dot). */
      num = ((int64_t)i_now * (int64_t)q_prev - (int64_t)q_now * (int64_t)i_prev) >> 31;
      den = ((int64_t)i_now * (int64_t)i_prev + (int64_t)q_now * (int64_t)q_prev) >> 31;

      /* Guard very small denominator to avoid unstable division spikes. */
      if (den > -1024 && den < 1024) {
        den = (den >= 0) ? 1024 : -1024;
        den_guard_count++;
      }

      /* bounded division in Q31 space; clamp denominator impact */
      {
        /*
         * Discriminator polarity here is inverted versus the generator convention,
         * so apply a negative sign. Use a stronger shift to keep usable audio level.
         */
        int64_t x = -((num << 29) / den);
        fm_q31 = sat_i32(x);
      }

      /* Light smoothing LPF before decimation. */
      audio_lp = sat_i32(((int64_t)audio_lp * 7 + fm_q31) / 8);

      decim_ctr++;
      if (decim_ctr >= DECIM) {
        int32_t y_q31;
        int32_t y_i16;
        /* 50 us de-emphasis in Q31 IIR form. */
        int64_t mixed = (int64_t)one_minus_alpha_q31 * audio_lp + (int64_t)deemph_alpha_q31 * deemph_state;
        deemph_state = sat_i32(mixed >> 31);
        y_q31 = deemph_state;
        y_i16 = y_q31 >> 15;
        /* Track basic health metrics for validation reports. */
        if (y_i16 == INT16_MAX || y_i16 == INT16_MIN) sat_count++;
        if (llabs((long long)y_i16) > peak_abs) peak_abs = llabs((long long)y_i16);
        {
          /* Emit one mono PCM16 sample (little-endian). */
          int16_t s = sat_i16(y_i16);
          uint8_t b0 = (uint8_t)(s & 0xFF);
          uint8_t b1 = (uint8_t)((s >> 8) & 0xFF);
          fwrite(&b0, 1, 1, fout);
          fwrite(&b1, 1, 1, fout);
        }
        count_out++;
        decim_ctr = 0;
      }
    }

    i_prev = i_now;
    q_prev = q_now;
  }

  /* Patch final WAV sizes now that output sample count is known. */
  {
    uint32_t data_bytes = (uint32_t)(count_out * 2U);
    fseek(fout, 0, SEEK_SET);
    write_wav_header(fout, FS_AUDIO, 1, 16, data_bytes);
  }

  fclose(fin);
  fclose(fout);

  /* Optional text report consumed by validation script. */
  if (frep) {
    fprintf(frep, "input_pairs=%llu\n", (unsigned long long)in_pairs);
    fprintf(frep, "output_samples=%llu\n", (unsigned long long)count_out);
    fprintf(frep, "peak_abs_i16=%lld\n", (long long)peak_abs);
    fprintf(frep, "sat_count=%llu\n", (unsigned long long)sat_count);
    fprintf(frep, "den_guard_count=%llu\n", (unsigned long long)den_guard_count);
    fclose(frep);
  }

  /* Console summary for quick manual checks. */
  printf("[fm_demod_proto] input=%s output=%s out_samples=%llu peak_i16=%lld\n",
         in_path,
         out_wav_path,
         (unsigned long long)count_out,
         (long long)peak_abs);
  return 0;
}
