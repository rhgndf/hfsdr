#include "ui/fft.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include <dsp/complex_math_functions.h>
#include <dsp/interpolation_functions.h>
#include <dsp/transform_functions.h>

extern "C" {
#include "debug.h"
#include "hw/i2s.h"
#include "hw/display/st7789.h"
}

#define FFT_SAMPLE_COUNT        I2S_HW_COMPLEX_SAMPLE_COUNT
#define FFT_COMPLEX_FLOAT_COUNT (FFT_SAMPLE_COUNT * 2U)
#define FFT_DISPLAY_SAMPLE_COUNT 240U
#define FFT_INTERP_COL_COUNT    FFT_SAMPLE_COUNT
#define FFT_INTERP_X_MAX        ((float32_t)(FFT_SAMPLE_COUNT - 1U) - 1.0e-3f)
#define FFT_UPDATE_PERIOD_MS    80U
#define FFT_MIN_POWER           1.0e-20f
#define FFT_DB_FLOOR            -200.0f
#define FFT_DB_CEILING          200.0f
#define FFT_DISPLAY_MIN_DB      -100.0f
#define FFT_DISPLAY_MAX_DB      -30.0f
#define FFT_WATERFALL_TOP       80U
#define FFT_WATERFALL_BOTTOM    320U

namespace {

constexpr float64_t kPi = 3.14159265358979323846264338327950288;

consteval float32_t blackman_harris_92db_sample(uint32_t index, uint32_t size)
{
    /* Fold the int32 -> float scale (1/2^31) into the window so the runtime
     * conversion loop drops one fmul per sample. */
    constexpr float64_t kScale = 1.0 / 2147483648.0;
    float64_t w = kPi * static_cast<float64_t>(index) * (2.0 / static_cast<float64_t>(size));
    return static_cast<float32_t>(kScale * (0.35875 -
                                            (0.48829 * std::cos(w)) +
                                            (0.14128 * std::cos(2.0 * w)) -
                                            (0.01168 * std::cos(3.0 * w))));
}

template<size_t Size>
consteval std::array<float32_t, Size> make_blackman_harris_92db_window()
{
    std::array<float32_t, Size> window{};
    for(size_t i = 0U; i < Size; ++i)
    {
        window[i] = blackman_harris_92db_sample(static_cast<uint32_t>(i), static_cast<uint32_t>(Size));
    }
    return window;
}

constexpr uint16_t kColorLutSize = 64U;

consteval uint16_t fft_color_lut_entry(uint32_t g)
{
    uint32_t intensity = g >> 1;
    uint32_t red = (intensity > 22U) ? ((intensity - 22U) * 7U) : 0U;
    if(red > 31U)
    {
        red = 31U;
    }
    uint32_t blue = 31U - intensity;
    return static_cast<uint16_t>((red << 11) | (g << 5) | blue);
}

consteval std::array<uint16_t, kColorLutSize> make_fft_color_lut()
{
    std::array<uint16_t, kColorLutSize> lut{};
    for(uint32_t g = 0U; g < kColorLutSize; ++g)
    {
        lut[g] = fft_color_lut_entry(g);
    }
    return lut;
}

}

static uint32_t s_last_rx_word_count = 0U;
static uint16_t s_waterfall_line = FFT_WATERFALL_TOP;
static riscv_cfft_instance_f32 s_fft_instance;
static riscv_linear_interp_instance_f32 s_fft_interp_instance;
static float32_t * const fft_buf = reinterpret_cast<float32_t * const>(i2s_fft_sample_arr);
static constexpr auto fft_window = make_blackman_harris_92db_window<FFT_SAMPLE_COUNT>();
static constexpr auto fft_color_lut = make_fft_color_lut();
static uint16_t * const fft_line = reinterpret_cast<uint16_t *>(&fft_buf[FFT_INTERP_COL_COUNT]);
static_assert(FFT_SAMPLE_COUNT == 256, "FFT sample count is not 256");
static_assert(sizeof(i2s_fft_sample_arr[0]) == sizeof(float32_t),
              "I2S and FFT buffers must use equal-width elements");
static_assert(alignof(int32_t) >= alignof(float32_t),
              "I2S buffer storage must satisfy float32_t alignment");
static_assert(((FFT_COMPLEX_FLOAT_COUNT - FFT_INTERP_COL_COUNT) * sizeof(float32_t)) >=
              (FFT_DISPLAY_SAMPLE_COUNT * sizeof(uint16_t)),
              "FFT line does not fit in the unused FFT buffer tail");

static uint16_t fft_db_to_color(float32_t db)
{
    float32_t normalized = std::clamp(
        (db - FFT_DISPLAY_MIN_DB) / (FFT_DISPLAY_MAX_DB - FFT_DISPLAY_MIN_DB),
        0.0f, 1.0f);

    uint32_t g = std::min(static_cast<uint32_t>(normalized * 63.0f),
                          static_cast<uint32_t>(kColorLutSize - 1U));
    return fft_color_lut[g];
}

/*
 * 10*log10(x) approximation via std::bit_cast log2 split.
 * - exponent extraction gives integer log2 for free.
 * - 4th-degree minimax (Remez) polynomial for log2 on [1,2], max error
 *   ~8.8e-5 -> ~0.0003 dB. Coefficients regenerable via tools/gen_log2_poly.py.
 * Handles x=0 (returns very negative; caller clamps to floor).
 */
static constexpr float32_t fast_10log10f(float32_t x)
{
    uint32_t bits = std::bit_cast<uint32_t>(x);
    int32_t e = static_cast<int32_t>((bits >> 23) & 0xFFU) - 127;
    float32_t m = std::bit_cast<float32_t>((bits & 0x007FFFFFu) | 0x3F800000u);
    float32_t l2m = -2.51285462f + (4.07009079f + (-2.12067513f + (0.64514236f - 0.08161581f * m) * m) * m) * m;
    /* 10 / log2(10) = 3.01029995664f */
    return (static_cast<float32_t>(e) + l2m) * 3.01029995664f;
}

namespace {
/* Verify polynomial against libm at compile time. __builtin_log10f is constexpr-foldable in GCC. */
consteval bool fast_10log10f_close(float32_t x, float32_t tol_db)
{
    float32_t got = fast_10log10f(x);
    float32_t want = 10.0f * __builtin_log10f(x);
    float32_t diff = got - want;
    return (diff > -tol_db) && (diff < tol_db);
}
static_assert(fast_10log10f_close(1.0f,    0.005f));
static_assert(fast_10log10f_close(10.0f,   0.005f));
static_assert(fast_10log10f_close(100.0f,  0.005f));
static_assert(fast_10log10f_close(0.1f,    0.005f));
static_assert(fast_10log10f_close(0.5f,    0.005f));
static_assert(fast_10log10f_close(1.5f,    0.005f));
static_assert(fast_10log10f_close(1e-10f,  0.005f));
static_assert(fast_10log10f_close(1e10f,   0.005f));
}

static float32_t fft_power_to_db(float32_t power)
{
    float32_t db = fast_10log10f(power);
    return std::clamp(db, FFT_DB_FLOOR, FFT_DB_CEILING);
}

static void fft_convert_and_window(void)
{
    /* kScale already folded into fft_window at compile time. */
    for(uint32_t i = 0U; i < FFT_SAMPLE_COUNT; ++i)
    {
        float32_t w = fft_window[i];
        fft_buf[2U * i] = static_cast<float32_t>(i2s_fft_sample_arr[2U * i]) * w;
        fft_buf[(2U * i) + 1U] = static_cast<float32_t>(i2s_fft_sample_arr[(2U * i) + 1U]) * w;
    }
}

static void fft_build_interp_db_table(void)
{
    for(uint32_t x_bin = FFT_SAMPLE_COUNT; x_bin > 0U; --x_bin)
    {
        uint32_t interp_bin = x_bin - 1U;
        uint32_t fft_bin = (interp_bin + (FFT_SAMPLE_COUNT / 2U)) % FFT_SAMPLE_COUNT;
        float32_t real = fft_buf[2U * fft_bin];
        float32_t imag = fft_buf[(2U * fft_bin) + 1U];
        fft_buf[interp_bin] = fft_power_to_db((real * real) + (imag * imag));
    }
}

void UI_FFT_Init(void)
{
    s_last_rx_word_count = 0U;
    s_waterfall_line = FFT_WATERFALL_TOP;

    if(riscv_cfft_init_256_f32(&s_fft_instance) != RISCV_MATH_SUCCESS)
    {
        return;
    }

    s_fft_interp_instance.nValues = FFT_INTERP_COL_COUNT;
    s_fft_interp_instance.x1 = 0.0f;
    s_fft_interp_instance.xSpacing = 1.0f;
    s_fft_interp_instance.pYData = fft_buf;
    i2s_fft_sample_arr_reset();

    s_waterfall_line = ST7789_ScrollRows(FFT_WATERFALL_TOP, FFT_WATERFALL_BOTTOM, 0);
}

void UI_FFT_Draw(void)
{
    uint32_t rx_word_count = i2s_hw_rx_word_count();

    if(rx_word_count == s_last_rx_word_count)
    {
        return;
    }
    if(!i2s_fft_sample_arr_ready())
    {
        return;
    }

    s_last_rx_word_count = rx_word_count;

    fft_convert_and_window();
    riscv_cfft_f32(&s_fft_instance, fft_buf, 0U, 1U);

    s_waterfall_line = ST7789_ScrollRows(FFT_WATERFALL_TOP, FFT_WATERFALL_BOTTOM, 1);
    ST7789_Fill(0U, s_waterfall_line, ST7789_WIDTH - 1U, s_waterfall_line, BLACK);

    fft_build_interp_db_table();

    constexpr float32_t source_x_step = FFT_INTERP_X_MAX /
                                        (float32_t)(FFT_DISPLAY_SAMPLE_COUNT - 1U);
    float32_t source_x = 0.0f;
    for(uint32_t x_bin = 0U; x_bin < FFT_DISPLAY_SAMPLE_COUNT; ++x_bin)
    {
        /* xSpacing == 1, x1 == 0, so the CMSIS linear interp reduces to a 2-tap blend. */
        uint32_t idx = static_cast<uint32_t>(source_x);
        float32_t frac = source_x - static_cast<float32_t>(idx);
        float32_t lo = fft_buf[idx];
        float32_t db = lo + frac * (fft_buf[idx + 1U] - lo);
        fft_line[x_bin] = fft_db_to_color(db);
        source_x += source_x_step;
    }
    ST7789_DrawColorLine(0U, s_waterfall_line, fft_line, FFT_DISPLAY_SAMPLE_COUNT);

    i2s_fft_sample_arr_reset();
}
