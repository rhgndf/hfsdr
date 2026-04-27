#include "ui/fft.h"

#include <array>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

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
#define FFT_INTERP_ROW_COUNT    2U
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
    float64_t w = kPi * static_cast<float64_t>(index) * (2.0 / static_cast<float64_t>(size));
    return static_cast<float32_t>(0.35875 -
                                  (0.48829 * std::cos(w)) +
                                  (0.14128 * std::cos(2.0 * w)) -
                                  (0.01168 * std::cos(3.0 * w)));
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

}

static uint32_t s_last_rx_word_count = 0U;
static uint16_t s_waterfall_line = FFT_WATERFALL_TOP;
static riscv_cfft_instance_f32 s_fft_instance;
static riscv_bilinear_interp_instance_f32 s_fft_interp_instance;
static constexpr auto fft_window = make_blackman_harris_92db_window<FFT_SAMPLE_COUNT>();
static uint16_t fft_line[FFT_DISPLAY_SAMPLE_COUNT];
static_assert(FFT_SAMPLE_COUNT == 256, "FFT sample count is not 256");

static uint64_t ui_fft_ticks_from_ms(uint32_t ms)
{
    uint64_t ticks = ((uint64_t)SystemCoreClock * (uint64_t)ms) / 1000ULL;
    if(ticks == 0U)
    {
        ticks = 1U;
    }
    return ticks;
}

static uint16_t fft_db_to_color(float32_t db)
{
    float32_t normalized = (db - FFT_DISPLAY_MIN_DB) / (FFT_DISPLAY_MAX_DB - FFT_DISPLAY_MIN_DB);

    if(normalized < 0.0f)
    {
        normalized = 0.0f;
    }
    else if(normalized > 1.0f)
    {
        normalized = 1.0f;
    }

    uint16_t intensity = (uint16_t)(normalized * 31.0f);
    uint16_t red = (intensity > 22U) ? (uint16_t)((intensity - 22U) * 7U) : 0U;
    uint16_t green = (uint16_t)(normalized * 63.0f);
    uint16_t blue = 31U - intensity;

    if(red > 31U)
    {
        red = 31U;
    }

    return (uint16_t)((red << 11) | (green << 5) | blue);
}

static float32_t fft_power_to_db(float32_t power)
{
    float32_t db = 10.0f * log10f(power + FFT_MIN_POWER);

    if(!isfinite(db))
    {
        return FFT_DB_CEILING;
    }

    if(db < FFT_DB_FLOOR)
    {
        return FFT_DB_FLOOR;
    }

    if(db > FFT_DB_CEILING)
    {
        return FFT_DB_CEILING;
    }

    return db;
}

static void fft_apply_window(void)
{
    riscv_cmplx_mult_real_f32(i2s_fft_sample_arr,
                              fft_window.data(),
                              i2s_fft_sample_arr,
                              FFT_SAMPLE_COUNT);
}

static void fft_build_interp_db_table(void)
{
    for(uint32_t x_bin = FFT_SAMPLE_COUNT; x_bin > 0U; --x_bin)
    {
        uint32_t interp_bin = x_bin - 1U;
        uint32_t fft_bin = (interp_bin + (FFT_SAMPLE_COUNT / 2U)) % FFT_SAMPLE_COUNT;
        float32_t real = i2s_fft_sample_arr[2U * fft_bin];
        float32_t imag = i2s_fft_sample_arr[(2U * fft_bin) + 1U];
        i2s_fft_sample_arr[interp_bin] = fft_power_to_db((real * real) + (imag * imag));
    }

    for(uint32_t x_bin = 0U; x_bin < FFT_INTERP_COL_COUNT; ++x_bin)
    {
        i2s_fft_sample_arr[FFT_INTERP_COL_COUNT + x_bin] = i2s_fft_sample_arr[x_bin];
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

    s_fft_interp_instance.numRows = FFT_INTERP_ROW_COUNT;
    s_fft_interp_instance.numCols = FFT_INTERP_COL_COUNT;
    s_fft_interp_instance.pData = i2s_fft_sample_arr;
    i2s_fft_sample_arr_reset();

    ST7789_Fill_Color(BLACK);
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

    fft_apply_window();
    riscv_cfft_f32(&s_fft_instance, i2s_fft_sample_arr, 0U, 1U);

    s_waterfall_line = ST7789_ScrollRows(FFT_WATERFALL_TOP, FFT_WATERFALL_BOTTOM, 1);
    ST7789_Fill(0U, s_waterfall_line, ST7789_WIDTH - 1U, s_waterfall_line, BLACK);


    fft_build_interp_db_table();

    for(uint32_t x_bin = 0U; x_bin < FFT_DISPLAY_SAMPLE_COUNT; ++x_bin)
    {
        float32_t source_x = ((float32_t)x_bin * FFT_INTERP_X_MAX) /
                             (float32_t)(FFT_DISPLAY_SAMPLE_COUNT - 1U);
        float32_t db = riscv_bilinear_interp_f32(&s_fft_interp_instance, source_x, 0.0f);
        fft_line[x_bin] = fft_db_to_color(db);
    }
    ST7789_DrawColorLine(0U, s_waterfall_line, fft_line, FFT_DISPLAY_SAMPLE_COUNT);

    i2s_fft_sample_arr_reset();
}
