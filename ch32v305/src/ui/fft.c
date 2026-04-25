#include "ui/fft.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "debug.h"
#include "hw/i2s_hw.h"
#include "hw/st7789/st7789.h"

typedef float float32_t;
typedef enum
{
    RISCV_MATH_SUCCESS = 0
} riscv_status;

typedef struct
{
    uint16_t fftLen;
    const float32_t *pTwiddle;
    const uint16_t *pBitRevTable;
    uint16_t bitRevLength;
} riscv_cfft_instance_f32;

riscv_status riscv_cfft_init_f32(riscv_cfft_instance_f32 *S, uint16_t fftLen);
void riscv_cfft_f32(const riscv_cfft_instance_f32 *S, float32_t *p1, uint8_t ifftFlag, uint8_t bitReverseFlag);
void riscv_blackman_harris_92db_f32(float32_t *pDst, uint32_t blockSize);

#define FFT_SAMPLE_COUNT        I2S_HW_COMPLEX_SAMPLE_COUNT
#define FFT_COMPLEX_FLOAT_COUNT (FFT_SAMPLE_COUNT * 2U)
#define FFT_UPDATE_PERIOD_MS    80U
#define FFT_MIN_POWER           1.0e-20f
#define FFT_DB_FLOOR            -200.0f
#define FFT_DB_CEILING          200.0f
#define FFT_DISPLAY_MIN_DB      -100.0f
#define FFT_DISPLAY_MAX_DB      -30.0f

static uint32_t s_last_rx_word_count = 0U;
static uint16_t s_line = 0U;
static riscv_cfft_instance_f32 s_fft_instance;
static float32_t s_fft_data[FFT_COMPLEX_FLOAT_COUNT];

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

void UI_FFT_Init(void)
{
    s_last_rx_word_count = 0U;
    s_line = 0U;

    if(riscv_cfft_init_f32(&s_fft_instance, FFT_SAMPLE_COUNT) != RISCV_MATH_SUCCESS)
    {
        return;
    }

    ST7789_Fill_Color(BLACK);
}

void UI_FFT_Draw(void)
{
    uint32_t rx_word_count = i2s_hw_rx_word_count();

    if(rx_word_count == s_last_rx_word_count)
    {
        return;
    }

    s_last_rx_word_count = rx_word_count;
    s_line = (uint16_t)((s_line + 1U) % ST7789_HEIGHT);

    float32_t fft_window[FFT_SAMPLE_COUNT];
    riscv_blackman_harris_92db_f32(fft_window, FFT_SAMPLE_COUNT);
    i2s_hw_obtain_buffer_and_window(s_fft_data, fft_window);
    riscv_cfft_f32(&s_fft_instance, s_fft_data, 0U, 1U);

    ST7789_Fill(0U, s_line, ST7789_WIDTH - 1U, s_line, BLACK);

    uint16_t fft_line[FFT_SAMPLE_COUNT];
    for(uint32_t x_bin = 0U; x_bin < FFT_SAMPLE_COUNT; ++x_bin)
    {
        uint32_t fft_bin = (x_bin + (FFT_SAMPLE_COUNT / 2U)) % FFT_SAMPLE_COUNT;
        float32_t real = s_fft_data[2U * fft_bin];
        float32_t imag = s_fft_data[(2U * fft_bin) + 1U];
        float32_t db = fft_power_to_db((real * real) + (imag * imag));
        fft_line[x_bin] = fft_db_to_color(db);
    }
    ST7789_DrawColorLine(0U, s_line, fft_line, FFT_SAMPLE_COUNT);
}
