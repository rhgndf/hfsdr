#include "ui/ui.h"

extern "C" {
#include "hw/dac.h"
#include "hw/display/splash.h"
#include "hw/encoder.h"
#include "hw/i2s.h"
#include "hw/si5351.h"
#include "hw/tlv320adc6120.h"
#include "hw/usb.h"
}

#include "demod/demod.h"
#include "hw/display/st7789.h"
#include "ui/fft.h"
#include "utils/utils.h"

#include "ch32v30x.h"
#include "system_ch32v30x.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#define UI_HEADER_BOTTOM_Y 79U
#define UI_FREQ_TEXT_Y     10U
#define UI_FREQ_DIGITS     9U
#define UI_FREQ_SUFFIX_CHARS 3U
#define UI_FREQ_TEXT_WIDTH ((UI_FREQ_DIGITS + UI_FREQ_SUFFIX_CHARS) * Font_16x26.width)
#define UI_FREQ_SUFFIX_X   ((UI_FREQ_DIGITS * Font_16x26.width) + (Font_16x26.width / 2U))
#define UI_MODE_TEXT_X     (ST7789_WIDTH - (4U * Font_11x18.width) - 1U)
#define UI_MODE_TEXT_Y     0U
#define UI_MODE_TEXT_WIDTH (4U * Font_11x18.width)
#define UI_MODE_TEXT_HEIGHT Font_11x18.height
#define UI_TRIANGLE_Y      1U
#define UI_VOL_ROW_Y       40U
#define UI_GAIN_ROW_Y      60U
#define UI_BAR_X           55U
#define UI_BAR_WIDTH       170U
#define UI_BAR_HEIGHT      12U
#define UI_VOLUME_MAX      100U
#define UI_VOLUME_DEFAULT  71U
#define UI_AUDIO_GAIN_MAX_Q16 (32UL << 8)
#define UI_TLV320_GAIN_MIN_DB_X2     TLV320ADC6120_CH_GAIN_MIN_DB_X2
#define UI_TLV320_GAIN_MAX_DB_X2     ((int8_t)TLV320ADC6120_CH_GAIN_MAX_DB_X2)
#define UI_TLV320_GAIN_DEFAULT_DB_X2 0

/* Match fft.cpp waterfall band (portrait). */
#define UI_WATERFALL_TOP     80U
#define UI_WATERFALL_BOTTOM  320U

/* Landscape splash_freq.png MHz readout quad (clockwise from top-left). */
#define UI_SPLASH_TEXT_X0    77U
#define UI_SPLASH_TEXT_Y0    24U
#define UI_SPLASH_TEXT_X1    167U
#define UI_SPLASH_TEXT_Y1    33U
#define UI_SPLASH_TEXT_X2    166U
#define UI_SPLASH_TEXT_Y2    74U
#define UI_SPLASH_TEXT_X3    76U
#define UI_SPLASH_TEXT_Y3    60U
#define UI_SPLASH_TEXT_BASELINE_DX  10
#define UI_SPLASH_TEXT_BASELINE_DY  1
#define UI_SPLASH_TEXT_SHEAR_NUM   (-1)
#define UI_SPLASH_TEXT_SHEAR_DEN   36
#define UI_SPLASH_FREQ_STEP_HZ 100000ULL

/* Landscape waveform scope quad (same vertex order as MHz quad). */
#define UI_SPLASH_WAVE_X0    188U
#define UI_SPLASH_WAVE_Y0    34U
#define UI_SPLASH_WAVE_X1    185U
#define UI_SPLASH_WAVE_Y1    102U
#define UI_SPLASH_WAVE_X2    314U
#define UI_SPLASH_WAVE_Y2    121U
#define UI_SPLASH_WAVE_X3    319U
#define UI_SPLASH_WAVE_Y3    48U
#define UI_SPLASH_WAVE_DISPLAY_COLS  128U
#define UI_SPLASH_WAVE_PERIOD_MS     36U
#define UI_SPLASH_WAVE_MIN_PEAK_ABS  120
#define UI_SPLASH_WAVE_PEAK_CEILING 2400 /* cap AGC so loud FM does not flatten trace */
#define UI_SPLASH_WAVE_DEFLECT_MUL   1600 /* 10x vs 320; paired with DIV along thickness vector */
#define UI_SPLASH_WAVE_DEFLECT_DIV   3072

/* Landscape spectrum quad (same vertex order as waveform quad). */
#define UI_SPLASH_SPEC_X0    186U
#define UI_SPLASH_SPEC_Y0    126U
#define UI_SPLASH_SPEC_X1    184U
#define UI_SPLASH_SPEC_Y1    176U
#define UI_SPLASH_SPEC_X2    314U
#define UI_SPLASH_SPEC_Y2    196U
#define UI_SPLASH_SPEC_X3    317U
#define UI_SPLASH_SPEC_Y3    145U
#define UI_SPLASH_SPEC_DISPLAY_COLS 128U
#define UI_SPLASH_SPEC_SAMPLE_RATE_HZ 192000
#define UI_SPLASH_SPEC_MIN_HZ (-20000)
#define UI_SPLASH_SPEC_MAX_HZ 30000
#define UI_SPLASH_SPEC_MIN_DB (-100.0f)
#define UI_SPLASH_SPEC_MAX_DB (0.0f)

typedef struct
{
    uint16_t x0;
    uint16_t y0;
    uint16_t x1;
    uint16_t y1;
    uint16_t x2;
    uint16_t y2;
    uint16_t x3;
    uint16_t y3;
} ui_splash_quad_t;

typedef enum
{
    UI_SPLASH_APPEARANCE_NORMAL,
    UI_SPLASH_APPEARANCE_INVERTED,
    UI_SPLASH_APPEARANCE_COUNT
} ui_splash_appearance_t;

typedef enum
{
    UI_DISPLAY_SPLASH,
    UI_DISPLAY_WATERFALL
} ui_display_mode_t;

static ui_display_mode_t s_display_mode = UI_DISPLAY_SPLASH;
static bool s_splash_band_dirty = true;
static ui_splash_appearance_t s_splash_appearance = UI_SPLASH_APPEARANCE_NORMAL;
static ui_splash_appearance_t s_displayed_splash_appearance = UI_SPLASH_APPEARANCE_COUNT;
static uint8_t s_splash_button_phase = 0U;
static uint64_t s_displayed_splash_freq_hz = UINT64_MAX;
static int32_t s_splash_wave_display_peak = (int32_t)UI_SPLASH_WAVE_MIN_PEAK_ABS;
static uint16_t s_splash_wave_prev_x[UI_SPLASH_WAVE_DISPLAY_COLS];
static uint16_t s_splash_wave_prev_y[UI_SPLASH_WAVE_DISPLAY_COLS];
static bool s_splash_wave_poly_valid = false;
static uint16_t s_splash_spec_prev_x[UI_SPLASH_SPEC_DISPLAY_COLS];
static uint16_t s_splash_spec_prev_y[UI_SPLASH_SPEC_DISPLAY_COLS];
static bool s_splash_spec_poly_valid = false;
/* Last MADCTL value applied by UI (0xFF = never synced this session). */
static uint8_t s_hw_madctl = 0xFFU;

static uint64_t s_displayed_freq_hz = UINT64_MAX;
static uint8_t s_displayed_volume = UINT8_MAX;
static int8_t s_displayed_tlv320_gain_db_x2 = INT8_MAX;
static demodulation_mode_t s_demod_mode = DEMODULATION_MODE_WBFM;
static demodulation_mode_t s_displayed_demod_mode = DEMODULATION_MODE_COUNT;
static uint8_t s_volume = UI_VOLUME_DEFAULT;
static int8_t s_tlv320_gain_db_x2 = UI_TLV320_GAIN_DEFAULT_DB_X2;
static bool s_redraw_all = true;

typedef enum
{
    UI_CONTROL_FREQ_10_MHZ,
    UI_CONTROL_FREQ_1_MHZ,
    UI_CONTROL_FREQ_100_KHZ,
    UI_CONTROL_FREQ_10_KHZ,
    UI_CONTROL_DEMOD_MODE,
    UI_CONTROL_VOLUME,
    UI_CONTROL_TLV320_GAIN,
    UI_CONTROL_COUNT
} ui_control_t;

static ui_control_t s_active_control = UI_CONTROL_FREQ_10_MHZ;
static ui_control_t s_displayed_active_control = UI_CONTROL_COUNT;

static bool ui_control_is_frequency(ui_control_t control)
{
    return control <= UI_CONTROL_FREQ_10_KHZ;
}

static uint64_t ui_frequency_step_hz(ui_control_t control)
{
    switch(control)
    {
        case UI_CONTROL_FREQ_10_KHZ:
            return 10000ULL;
        case UI_CONTROL_FREQ_100_KHZ:
            return 100000ULL;
        case UI_CONTROL_FREQ_1_MHZ:
            return 1000000ULL;
        case UI_CONTROL_FREQ_10_MHZ:
            return 10000000ULL;
        default:
            return 100000ULL;
    }
}

static uint8_t ui_frequency_digit_power(ui_control_t control)
{
    switch(control)
    {
        case UI_CONTROL_FREQ_10_KHZ:
            return 4U;
        case UI_CONTROL_FREQ_100_KHZ:
            return 5U;
        case UI_CONTROL_FREQ_1_MHZ:
            return 6U;
        case UI_CONTROL_FREQ_10_MHZ:
            return 7U;
        default:
            return 5U;
    }
}

static uint64_t ui_apply_frequency_delta(uint64_t freq_hz, int16_t delta, uint64_t step_hz)
{
    int64_t next_freq_hz = (int64_t)freq_hz + ((int64_t)delta * (int64_t)step_hz);

    if(next_freq_hz < (int64_t)SI5351_MIN_OUTPUT_HZ)
    {
        next_freq_hz = (int64_t)SI5351_MIN_OUTPUT_HZ;
    }

    return (uint64_t)next_freq_hz;
}

static uint8_t ui_apply_delta_u8(uint8_t value, int16_t delta, uint8_t min, uint8_t max)
{
    int16_t next = (int16_t)value + delta;

    if(next < (int16_t)min)
    {
        return min;
    }
    if(next > (int16_t)max)
    {
        return max;
    }

    return (uint8_t)next;
}

static int8_t ui_apply_delta_i8(int8_t value, int16_t delta, int8_t min, int8_t max)
{
    int16_t next = (int16_t)value + delta;

    if(next < (int16_t)min)
    {
        return min;
    }
    if(next > (int16_t)max)
    {
        return max;
    }

    return (int8_t)next;
}

static uint32_t ui_volume_gain_q16(uint8_t volume)
{
    uint64_t volume_squared = (uint64_t)volume * (uint64_t)volume;
    uint64_t max_squared = (uint64_t)UI_VOLUME_MAX * (uint64_t)UI_VOLUME_MAX;

    return (uint32_t)(((uint64_t)UI_AUDIO_GAIN_MAX_Q16 * volume_squared) / max_squared);
}

static void ui_splash_exit_to_waterfall(void)
{
    s_display_mode = UI_DISPLAY_WATERFALL;
    s_active_control = UI_CONTROL_FREQ_10_MHZ;
    s_splash_band_dirty = false;
    s_splash_button_phase = 0U;
    s_displayed_splash_freq_hz = UINT64_MAX;
    s_displayed_splash_appearance = UI_SPLASH_APPEARANCE_COUNT;
    s_redraw_all = true;
    s_displayed_active_control = UI_CONTROL_COUNT;
    s_splash_wave_display_peak = (int32_t)UI_SPLASH_WAVE_MIN_PEAK_ABS;
    s_splash_wave_poly_valid = false;
    s_splash_spec_poly_valid = false;
}

static void ui_handle_button_press(void)
{
    if(s_display_mode == UI_DISPLAY_SPLASH)
    {
        s_splash_button_phase = (uint8_t)((s_splash_button_phase + 1U) % 3U);

        if(s_splash_button_phase == 0U)
        {
            ui_splash_exit_to_waterfall();
            return;
        }

        s_splash_appearance = (s_splash_button_phase == 1U) ? UI_SPLASH_APPEARANCE_INVERTED
                                                            : UI_SPLASH_APPEARANCE_NORMAL;
        s_splash_band_dirty = true;
        s_displayed_splash_freq_hz = UINT64_MAX;
        s_displayed_splash_appearance = UI_SPLASH_APPEARANCE_COUNT;
        return;
    }

    if(s_active_control == UI_CONTROL_TLV320_GAIN)
    {
        s_display_mode = UI_DISPLAY_SPLASH;
        s_splash_band_dirty = true;
        s_splash_button_phase = 0U;
        s_displayed_splash_freq_hz = UINT64_MAX;
        s_displayed_splash_appearance = UI_SPLASH_APPEARANCE_COUNT;
        return;
    }

    s_active_control = (ui_control_t)((uint32_t)s_active_control + 1U);
}

static void ui_draw_selected_frequency_digit(uint64_t freq_hz)
{
    uint8_t digit_power = ui_frequency_digit_power(s_active_control);
    uint16_t digit_index = (uint16_t)(UI_FREQ_DIGITS - 1U - digit_power);

    (void)freq_hz;

    uint16_t center_x = (uint16_t)((digit_index * Font_16x26.width) + (Font_16x26.width / 2U));
    ST7789_DrawFilledTriangle((uint16_t)(center_x - 5U), UI_TRIANGLE_Y,
                              (uint16_t)(center_x + 5U), UI_TRIANGLE_Y,
                              center_x, (uint16_t)(UI_TRIANGLE_Y + 7U),
                              YELLOW);
}

static void ui_draw_frequency(uint64_t freq_hz)
{
    char freq_text[24];

    snprintf(freq_text, sizeof(freq_text), "%9lu", (unsigned long)freq_hz);

    ST7789_Fill(0U, 0U, (uint16_t)(UI_FREQ_TEXT_WIDTH - 1U), (uint16_t)(UI_FREQ_TEXT_Y - 1U), BLACK);
    ST7789_WriteString(0U, UI_FREQ_TEXT_Y, freq_text, Font_16x26, WHITE, BLACK);
    ST7789_WriteString(UI_FREQ_SUFFIX_X, UI_FREQ_TEXT_Y, "Hz", Font_16x26, WHITE, BLACK);
    if(ui_control_is_frequency(s_active_control))
    {
        ui_draw_selected_frequency_digit(freq_hz);
    }
}

static char const *ui_demod_mode_text(void)
{
    switch(s_demod_mode)
    {
        case DEMODULATION_MODE_NBFM:
            return "NBFM";

        case DEMODULATION_MODE_AM:
            return " AM ";

        case DEMODULATION_MODE_USB:
            return "USB ";

        case DEMODULATION_MODE_LSB:
            return "LSB ";

        case DEMODULATION_MODE_WBFM:
        default:
            return "WBFM";
    }
}

static void ui_draw_mode_control(void)
{
    bool active = (s_active_control == UI_CONTROL_DEMOD_MODE);
    uint16_t fg = active ? BLACK : CYAN;
    uint16_t bg = active ? CYAN : BLACK;

    ST7789_Fill(UI_MODE_TEXT_X,
                UI_MODE_TEXT_Y,
                (uint16_t)(UI_MODE_TEXT_X + UI_MODE_TEXT_WIDTH - 1U),
                (uint16_t)(UI_MODE_TEXT_Y + UI_MODE_TEXT_HEIGHT - 1U),
                bg);
    ST7789_WriteString(UI_MODE_TEXT_X, UI_MODE_TEXT_Y, ui_demod_mode_text(), Font_11x18, fg, bg);
}

static void ui_draw_progress_row(uint16_t y, char const *label, int16_t value, int16_t min, int16_t max, bool active)
{
    uint16_t color = active ? YELLOW : WHITE;
    uint16_t fill_color = active ? YELLOW : GREEN;
    uint16_t bar_y = (uint16_t)(y + 3U);
    uint16_t inner_width = UI_BAR_WIDTH - 2U;
    uint16_t filled_width = 0U;

    if(max > min)
    {
        int16_t clamped = value;

        if(clamped < min)
        {
            clamped = min;
        }
        if(clamped > max)
        {
            clamped = max;
        }

        filled_width = (uint16_t)(((uint32_t)inner_width * (uint32_t)(clamped - min)) / (uint32_t)(max - min));
    }

    ST7789_WriteString(0U, y, label, Font_11x18, color, BLACK);
    ST7789_DrawRectangle(UI_BAR_X,
                         bar_y,
                         (uint16_t)(UI_BAR_X + UI_BAR_WIDTH - 1U),
                         (uint16_t)(bar_y + UI_BAR_HEIGHT - 1U),
                         color);
    ST7789_Fill((uint16_t)(UI_BAR_X + 1U),
                (uint16_t)(bar_y + 1U),
                (uint16_t)(UI_BAR_X + UI_BAR_WIDTH - 2U),
                (uint16_t)(bar_y + UI_BAR_HEIGHT - 2U),
                BLACK);
    if(filled_width != 0U)
    {
        ST7789_Fill((uint16_t)(UI_BAR_X + 1U),
                    (uint16_t)(bar_y + 1U),
                    (uint16_t)(UI_BAR_X + filled_width),
                    (uint16_t)(bar_y + UI_BAR_HEIGHT - 2U),
                    fill_color);
    }
}

static void ui_draw_header(uint64_t freq_hz)
{
    if(s_redraw_all)
    {
        ST7789_Fill(0U, 0U, ST7789_WIDTH - 1U, UI_HEADER_BOTTOM_Y, BLACK);
    }

    ui_draw_mode_control();
    ui_draw_frequency(freq_hz);
    ui_draw_progress_row(UI_VOL_ROW_Y,
                         "VOL",
                         s_volume,
                         0,
                         UI_VOLUME_MAX,
                         s_active_control == UI_CONTROL_VOLUME);
    ui_draw_progress_row(UI_GAIN_ROW_Y,
                         "GAIN",
                         s_tlv320_gain_db_x2,
                         UI_TLV320_GAIN_MIN_DB_X2,
                         UI_TLV320_GAIN_MAX_DB_X2,
                         s_active_control == UI_CONTROL_TLV320_GAIN);

    s_displayed_freq_hz = freq_hz;
    s_displayed_volume = s_volume;
    s_displayed_tlv320_gain_db_x2 = s_tlv320_gain_db_x2;
    s_displayed_demod_mode = s_demod_mode;
    s_displayed_active_control = s_active_control;
    s_redraw_all = false;
}

static void ui_sync_display_hw_for_mode(void)
{
    uint8_t const want_madctl = (s_display_mode == UI_DISPLAY_SPLASH) ? 3U : (uint8_t)ST7789_ROTATION;

    if(s_hw_madctl == want_madctl)
    {
        return;
    }

    uint8_t const prev_madctl = s_hw_madctl;

    s_hw_madctl = want_madctl;
    ST7789_SetRotation(want_madctl);

    if(want_madctl == 3U)
    {
        /* Landscape: full-screen PCB splash; turn off FFT scroll window first. */
        ST7789_VerticalScrollDisable();
        s_splash_band_dirty = true;
        return;
    }

    /* Portrait: radio header + waterfall. */
    UI_FFT_Init();
    if(prev_madctl == 3U)
    {
        ST7789_Fill(0U,
                    UI_WATERFALL_TOP,
                    ST7789_WIDTH - 1U,
                    (uint16_t)(UI_WATERFALL_BOTTOM - 1U),
                    BLACK);
    }
    s_redraw_all = true;
}

static void ui_splash_colors(uint16_t *fg, uint16_t *bg)
{
    if(s_splash_appearance == UI_SPLASH_APPEARANCE_INVERTED)
    {
        *fg = BLACK;
        *bg = WHITE;
    }
    else
    {
        *fg = WHITE;
        *bg = BLACK;
    }
}

static int32_t ui_splash_edge_x_at_y(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t y, bool *valid)
{
    *valid = false;

    if(y0 == y1)
    {
        if(y == y0)
        {
            *valid = true;
            return x0;
        }

        return 0;
    }

    int32_t y_min = (y0 < y1) ? y0 : y1;
    int32_t y_max = (y0 > y1) ? y0 : y1;

    if((y < y_min) || (y > y_max))
    {
        return 0;
    }

    *valid = true;
    return x0 + ((y - y0) * (x1 - x0)) / (y1 - y0);
}

static void ui_splash_quad_scanline_bounds(const ui_splash_quad_t *quad, int32_t y, int32_t *x_lo, int32_t *x_hi)
{
    int32_t xs[4];
    int32_t n = 0;

    bool valid;
    int32_t x = ui_splash_edge_x_at_y((int32_t)quad->x0,
                                      (int32_t)quad->y0,
                                      (int32_t)quad->x1,
                                      (int32_t)quad->y1,
                                      y,
                                      &valid);
    if(valid)
    {
        xs[n++] = x;
    }

    x = ui_splash_edge_x_at_y((int32_t)quad->x1,
                              (int32_t)quad->y1,
                              (int32_t)quad->x2,
                              (int32_t)quad->y2,
                              y,
                              &valid);
    if(valid)
    {
        xs[n++] = x;
    }

    x = ui_splash_edge_x_at_y((int32_t)quad->x2,
                              (int32_t)quad->y2,
                              (int32_t)quad->x3,
                              (int32_t)quad->y3,
                              y,
                              &valid);
    if(valid)
    {
        xs[n++] = x;
    }

    x = ui_splash_edge_x_at_y((int32_t)quad->x3,
                              (int32_t)quad->y3,
                              (int32_t)quad->x0,
                              (int32_t)quad->y0,
                              y,
                              &valid);
    if(valid)
    {
        xs[n++] = x;
    }

    if(n < 2)
    {
        *x_lo = 0;
        *x_hi = -1;
        return;
    }

    for(int32_t i = 0; i < (n - 1); ++i)
    {
        for(int32_t j = (i + 1); j < n; ++j)
        {
            if(xs[j] < xs[i])
            {
                int32_t tmp = xs[i];
                xs[i] = xs[j];
                xs[j] = tmp;
            }
        }
    }

    *x_lo = xs[0];
    *x_hi = xs[n - 1];
}

static uint16_t ui_splash_quad_y_min(const ui_splash_quad_t *quad)
{
    uint16_t m = quad->y0;

    if(quad->y1 < m)
    {
        m = quad->y1;
    }
    if(quad->y2 < m)
    {
        m = quad->y2;
    }
    if(quad->y3 < m)
    {
        m = quad->y3;
    }

    return m;
}

static uint16_t ui_splash_quad_y_max(const ui_splash_quad_t *quad)
{
    uint16_t m = quad->y0;

    if(quad->y1 > m)
    {
        m = quad->y1;
    }
    if(quad->y2 > m)
    {
        m = quad->y2;
    }
    if(quad->y3 > m)
    {
        m = quad->y3;
    }

    return m;
}

static void ui_splash_fill_quad(const ui_splash_quad_t *quad, uint16_t color)
{
    uint16_t y_min = ui_splash_quad_y_min(quad);
    uint16_t y_max = ui_splash_quad_y_max(quad);

    for(int32_t y = (int32_t)y_min; y <= (int32_t)y_max; ++y)
    {
        int32_t x_lo;
        int32_t x_hi;

        ui_splash_quad_scanline_bounds(quad, y, &x_lo, &x_hi);

        if(x_hi < x_lo)
        {
            continue;
        }

        if(x_lo < 0)
        {
            x_lo = 0;
        }

        ST7789_Fill((uint16_t)x_lo, (uint16_t)y, (uint16_t)x_hi, (uint16_t)y, color);
    }
}

static const ui_splash_quad_t s_splash_quad_mhz = {
    UI_SPLASH_TEXT_X0, UI_SPLASH_TEXT_Y0,
    UI_SPLASH_TEXT_X1, UI_SPLASH_TEXT_Y1,
    UI_SPLASH_TEXT_X2, UI_SPLASH_TEXT_Y2,
    UI_SPLASH_TEXT_X3, UI_SPLASH_TEXT_Y3
};

static const ui_splash_quad_t s_splash_quad_wave = {
    UI_SPLASH_WAVE_X0, UI_SPLASH_WAVE_Y0,
    UI_SPLASH_WAVE_X1, UI_SPLASH_WAVE_Y1,
    UI_SPLASH_WAVE_X2, UI_SPLASH_WAVE_Y2,
    UI_SPLASH_WAVE_X3, UI_SPLASH_WAVE_Y3
};

static const ui_splash_quad_t s_splash_quad_spec = {
    UI_SPLASH_SPEC_X0, UI_SPLASH_SPEC_Y0,
    UI_SPLASH_SPEC_X1, UI_SPLASH_SPEC_Y1,
    UI_SPLASH_SPEC_X2, UI_SPLASH_SPEC_Y2,
    UI_SPLASH_SPEC_X3, UI_SPLASH_SPEC_Y3
};

static uint16_t ui_splash_clamp_u16(int32_t v, uint16_t max_u)
{
    if(v < 0)
    {
        return 0U;
    }
    if(v > (int32_t)max_u)
    {
        return max_u;
    }

    return (uint16_t)v;
}


static int32_t ui_waveform_get(size_t idx)
{
    const volatile uint32_t* buf = dac_hw_stream_ring_samples();
    const volatile uint16_t* buf_idx = (const volatile uint16_t*)&buf[idx];
    return *buf_idx;
}

static void ui_draw_splash_waveform(void)
{
    size_t const display_cols = (size_t)UI_SPLASH_WAVE_DISPLAY_COLS;
    uint16_t trace_color;
    uint16_t scope_fill;

    ui_splash_colors(&scope_fill, &trace_color);

    size_t n = 2048;
    if((n < 2U) || (display_cols < 2U))
    {
        s_splash_wave_poly_valid = false;
        return;
    }

    int32_t frame_peak = (int32_t)UI_SPLASH_WAVE_MIN_PEAK_ABS;
    for(size_t i = 0U; i < n; ++i)
    {
        int32_t raw = (int32_t)ui_waveform_get(i) - 2048;
        if(raw < 0)
        {
            raw = -raw;
        }
        if(raw > frame_peak)
        {
            frame_peak = raw;
        }
    }

    if(frame_peak > s_splash_wave_display_peak)
    {
        s_splash_wave_display_peak = frame_peak;
    }
    else
    {
        int32_t decay = s_splash_wave_display_peak / 24;
        if(decay < 6)
        {
            decay = 6;
        }
        s_splash_wave_display_peak -= decay;
        if(s_splash_wave_display_peak < (int32_t)UI_SPLASH_WAVE_MIN_PEAK_ABS)
        {
            s_splash_wave_display_peak = (int32_t)UI_SPLASH_WAVE_MIN_PEAK_ABS;
        }
    }

    if(s_splash_wave_display_peak > (int32_t)UI_SPLASH_WAVE_PEAK_CEILING)
    {
        s_splash_wave_display_peak = (int32_t)UI_SPLASH_WAVE_PEAK_CEILING;
    }

    uint16_t w_lim = (uint16_t)(ST7789_GetWidth() - 1U);
    uint16_t h_lim = (uint16_t)(ST7789_GetHeight() - 1U);

    int32_t den_plot = (int32_t)(display_cols - 1U);
    int32_t const dx_top = (int32_t)UI_SPLASH_WAVE_X3 - (int32_t)UI_SPLASH_WAVE_X0;
    int32_t const dy_top = (int32_t)UI_SPLASH_WAVE_Y3 - (int32_t)UI_SPLASH_WAVE_Y0;
    int32_t const dx_bot = (int32_t)UI_SPLASH_WAVE_X2 - (int32_t)UI_SPLASH_WAVE_X1;
    int32_t const dy_bot = (int32_t)UI_SPLASH_WAVE_Y2 - (int32_t)UI_SPLASH_WAVE_Y1;

    if(s_splash_wave_poly_valid)
    {
        for(size_t i = 1U; i < display_cols; ++i)
        {
            ST7789_DrawLineFills(s_splash_wave_prev_x[i - 1U],
                                 s_splash_wave_prev_y[i - 1U],
                                 s_splash_wave_prev_x[i],
                                 s_splash_wave_prev_y[i],
                                 scope_fill);
        }
    }
    else
    {
        ui_splash_fill_quad(&s_splash_quad_wave, scope_fill);
    }

    uint16_t prev_px = 0U;
    uint16_t prev_py = 0U;
    for(size_t i = 0U; i < display_cols; ++i)
    {
        int32_t j = ((int32_t)i * (int32_t)(n - 1U)) / den_plot;
        uint32_t jm = (uint32_t)j > 0U ? (uint32_t)j - 1U : 0U;
        uint32_t jp = (uint32_t)j + 1U < n ? (uint32_t)j + 1U : (uint32_t)(n - 1U);
        int32_t sum3 = (int32_t)ui_waveform_get(jm) + (int32_t)ui_waveform_get(j) + (int32_t)ui_waveform_get(jp);
        int32_t raw = (sum3 / 3) - 2048;
        int32_t amp = (raw * (int32_t)UI_SPLASH_WAVE_DEFLECT_MUL) / s_splash_wave_display_peak;

        int32_t x_top = (int32_t)UI_SPLASH_WAVE_X0 + (((int32_t)i * dx_top) / den_plot);
        int32_t y_top = (int32_t)UI_SPLASH_WAVE_Y0 + (((int32_t)i * dy_top) / den_plot);
        int32_t x_bot = (int32_t)UI_SPLASH_WAVE_X1 + (((int32_t)i * dx_bot) / den_plot);
        int32_t y_bot = (int32_t)UI_SPLASH_WAVE_Y1 + (((int32_t)i * dy_bot) / den_plot);
        int32_t x_center = (x_top + x_bot) / 2;
        int32_t y_center = (y_top + y_bot) / 2;
        int32_t vx = x_bot - x_top;
        int32_t vy = y_bot - y_top;

        int32_t px = x_center + ((vx * amp) / (int32_t)UI_SPLASH_WAVE_DEFLECT_DIV);
        int32_t py = y_center + ((vy * amp) / (int32_t)UI_SPLASH_WAVE_DEFLECT_DIV);

        uint16_t curr_px = ui_splash_clamp_u16(px, w_lim);
        uint16_t curr_py = ui_splash_clamp_u16(py, h_lim);
        if(i > 0U)
        {
            ST7789_DrawLineFills(prev_px, prev_py, curr_px, curr_py, trace_color);
        }
        s_splash_wave_prev_x[i] = curr_px;
        s_splash_wave_prev_y[i] = curr_py;
        prev_px = curr_px;
        prev_py = curr_py;
    }

    s_splash_wave_poly_valid = true;
}

static float ui_splash_spec_fft_bin_at_hz(int32_t hz, uint32_t bin_count)
{
    return (((float)hz + ((float)UI_SPLASH_SPEC_SAMPLE_RATE_HZ * 0.5f)) * (float)bin_count) /
           (float)UI_SPLASH_SPEC_SAMPLE_RATE_HZ;
}

static float ui_splash_spec_db_at_bin(const float *fft_buf, uint32_t bin_count, float bin)
{
    if(bin < 0.0f)
    {
        bin = 0.0f;
    }

    float max_bin = (float)(bin_count - 2U);
    if(bin > max_bin)
    {
        bin = max_bin;
    }

    uint32_t idx = (uint32_t)bin;
    float frac = bin - (float)idx;

    return fft_buf[idx] + (frac * (fft_buf[idx + 1U] - fft_buf[idx]));
}

static uint16_t ui_splash_spec_y_frac(float db)
{
    float normalized = (db - UI_SPLASH_SPEC_MIN_DB) / (UI_SPLASH_SPEC_MAX_DB - UI_SPLASH_SPEC_MIN_DB);

    if(normalized < 0.0f)
    {
        normalized = 0.0f;
    }
    if(normalized > 1.0f)
    {
        normalized = 1.0f;
    }

    return (uint16_t)(normalized * 1024.0f);
}

static void ui_draw_splash_spectrum(void)
{
    uint16_t trace_color;
    uint16_t scope_fill;

    ui_splash_colors(&scope_fill, &trace_color);

    if(!s_splash_spec_poly_valid)
    {
        ui_splash_fill_quad(&s_splash_quad_spec, scope_fill);
    }

    UI_FFT_Compute();

    const float *fft_buf = UI_FFT_Buffer();
    uint32_t const bin_count = UI_FFT_BinCount();
    if((fft_buf == nullptr) || (bin_count < 2U))
    {
        s_splash_spec_poly_valid = false;
        return;
    }

    size_t const display_cols = (size_t)UI_SPLASH_SPEC_DISPLAY_COLS;
    if(s_splash_spec_poly_valid)
    {
        for(size_t i = 1U; i < display_cols; ++i)
        {
            ST7789_DrawLineFills(s_splash_spec_prev_x[i - 1U],
                                 s_splash_spec_prev_y[i - 1U],
                                 s_splash_spec_prev_x[i],
                                 s_splash_spec_prev_y[i],
                                 scope_fill);
        }
    }

    uint16_t w_lim = (uint16_t)(ST7789_GetWidth() - 1U);
    uint16_t h_lim = (uint16_t)(ST7789_GetHeight() - 1U);
    int32_t den_plot = (int32_t)(display_cols - 1U);
    int32_t const dx_top = (int32_t)UI_SPLASH_SPEC_X3 - (int32_t)UI_SPLASH_SPEC_X0;
    int32_t const dy_top = (int32_t)UI_SPLASH_SPEC_Y3 - (int32_t)UI_SPLASH_SPEC_Y0;
    int32_t const dx_bot = (int32_t)UI_SPLASH_SPEC_X2 - (int32_t)UI_SPLASH_SPEC_X1;
    int32_t const dy_bot = (int32_t)UI_SPLASH_SPEC_Y2 - (int32_t)UI_SPLASH_SPEC_Y1;
    float const bin_min = ui_splash_spec_fft_bin_at_hz(UI_SPLASH_SPEC_MIN_HZ, bin_count);
    float const bin_max = ui_splash_spec_fft_bin_at_hz(UI_SPLASH_SPEC_MAX_HZ, bin_count);
    float const bin_step = (bin_max - bin_min) / (float)den_plot;
    float bin = bin_min;

    uint16_t prev_px = 0U;
    uint16_t prev_py = 0U;
    for(size_t i = 0U; i < display_cols; ++i)
    {
        int32_t x_top = (int32_t)UI_SPLASH_SPEC_X0 + (((int32_t)i * dx_top) / den_plot);
        int32_t y_top = (int32_t)UI_SPLASH_SPEC_Y0 + (((int32_t)i * dy_top) / den_plot);
        int32_t x_bot = (int32_t)UI_SPLASH_SPEC_X1 + (((int32_t)i * dx_bot) / den_plot);
        int32_t y_bot = (int32_t)UI_SPLASH_SPEC_Y1 + (((int32_t)i * dy_bot) / den_plot);
        uint16_t y_frac = ui_splash_spec_y_frac(ui_splash_spec_db_at_bin(fft_buf, bin_count, bin));
        int32_t px = x_bot + (((x_top - x_bot) * (int32_t)y_frac) / 1024);
        int32_t py = y_bot + (((y_top - y_bot) * (int32_t)y_frac) / 1024);
        uint16_t curr_px = ui_splash_clamp_u16(px, w_lim);
        uint16_t curr_py = ui_splash_clamp_u16(py, h_lim);

        if(i > 0U)
        {
            ST7789_DrawLineFills(prev_px, prev_py, curr_px, curr_py, trace_color);
        }

        s_splash_spec_prev_x[i] = curr_px;
        s_splash_spec_prev_y[i] = curr_py;
        prev_px = curr_px;
        prev_py = curr_py;
        bin += bin_step;
    }

    s_splash_spec_poly_valid = true;
    i2s_fft_sample_arr_reset();
}

static void ui_draw_splash_freq_overlay(uint64_t freq_hz)
{
    char text[20];
    unsigned long mhz = (unsigned long)(freq_hz / 1000000ULL);
    unsigned long mhz_frac = (unsigned long)((freq_hz % 1000000ULL) / 100000ULL);

    snprintf(text, sizeof(text), "%lu.%lu MHz", mhz, mhz_frac);

    uint16_t text_fg = (s_splash_appearance == UI_SPLASH_APPEARANCE_INVERTED) ? BLACK : WHITE;
    uint16_t text_bg = (s_splash_appearance == UI_SPLASH_APPEARANCE_INVERTED) ? WHITE : BLACK;

    ui_splash_fill_quad(&s_splash_quad_mhz, text_bg);

    size_t text_len = strlen(text);
    int32_t top_span_x = (int32_t)UI_SPLASH_TEXT_X1 - (int32_t)UI_SPLASH_TEXT_X0;
    int32_t text_span_x = (int32_t)text_len * UI_SPLASH_TEXT_BASELINE_DX;
    int32_t start_x = (int32_t)UI_SPLASH_TEXT_X0 + ((top_span_x - text_span_x) / 2);
    int32_t start_y = (int32_t)UI_SPLASH_TEXT_Y0 + 12;

    if(top_span_x > 0)
    {
        start_y += (((top_span_x - text_span_x) / 2) * ((int32_t)UI_SPLASH_TEXT_Y1 - (int32_t)UI_SPLASH_TEXT_Y0)) /
                   top_span_x;
    }

    ST7789_WriteStringSlanted(start_x,
                              start_y,
                              text,
                              Font_11x18,
                              text_fg,
                              UI_SPLASH_TEXT_BASELINE_DX,
                              UI_SPLASH_TEXT_BASELINE_DY,
                              UI_SPLASH_TEXT_SHEAR_NUM,
                              UI_SPLASH_TEXT_SHEAR_DEN);
}

static void ui_draw_splash_fullscreen_landscape(uint64_t freq_hz)
{
    s_splash_wave_poly_valid = false;
    s_splash_spec_poly_valid = false;

    uint16_t bitmap_fg;
    uint16_t bitmap_bg;

    ui_splash_colors(&bitmap_fg, &bitmap_bg);

    ST7789_Fill_Color(bitmap_bg);
    ST7789_DrawBitmap1bpp(0U,
                          0U,
                          splash_freq_screen_w,
                          splash_freq_screen_h,
                          splash_freq_screen,
                          bitmap_fg,
                          bitmap_bg);
    ui_draw_splash_freq_overlay(freq_hz);
    ui_draw_splash_waveform();
    ui_draw_splash_spectrum();
}

static void ui_apply_encoder_delta(int16_t delta, uint64_t freq_hz, uint64_t *next_freq_hz)
{
    *next_freq_hz = freq_hz;

    if(delta == 0)
    {
        return;
    }

    switch(s_active_control)
    {
        case UI_CONTROL_FREQ_10_KHZ:
        case UI_CONTROL_FREQ_100_KHZ:
        case UI_CONTROL_FREQ_1_MHZ:
        case UI_CONTROL_FREQ_10_MHZ:
        {
            uint64_t requested_freq_hz = ui_apply_frequency_delta(freq_hz, delta, ui_frequency_step_hz(s_active_control));

            if(usb_hw_set_clk_freq_hz(requested_freq_hz) == READY)
            {
                *next_freq_hz = si5351_hw_clk0_get_freq_hz();
            }
            break;
        }

        case UI_CONTROL_VOLUME:
        {
            uint8_t volume = ui_apply_delta_u8(s_volume, delta, 0U, UI_VOLUME_MAX);

            if(volume != s_volume)
            {
                s_volume = volume;
                demod_set_gain(ui_volume_gain_q16(s_volume));
            }
            break;
        }

        case UI_CONTROL_DEMOD_MODE:
        {
            if(delta > 0)
            {
                s_demod_mode = (s_demod_mode == (demodulation_mode_t)(DEMODULATION_MODE_COUNT - 1))
                    ? DEMODULATION_MODE_WBFM
                    : (demodulation_mode_t)(s_demod_mode + 1);
            }
            else
            {
                s_demod_mode = (s_demod_mode == DEMODULATION_MODE_WBFM)
                    ? (demodulation_mode_t)(DEMODULATION_MODE_COUNT - 1)
                    : (demodulation_mode_t)(s_demod_mode - 1);
            }

            demod_set_mode(s_demod_mode);
            break;
        }

        case UI_CONTROL_TLV320_GAIN:
        {
            int8_t gain_db_x2 = ui_apply_delta_i8(s_tlv320_gain_db_x2, delta, UI_TLV320_GAIN_MIN_DB_X2, UI_TLV320_GAIN_MAX_DB_X2);

            if((gain_db_x2 != s_tlv320_gain_db_x2) && (usb_hw_set_tlv320_gain_db_x2(gain_db_x2) == READY))
            {
                s_tlv320_gain_db_x2 = gain_db_x2;
            }
            break;
        }

        case UI_CONTROL_COUNT:
        default:
            break;
    }
}

void UI_Init(void)
{
    s_displayed_freq_hz = UINT64_MAX;
    s_displayed_volume = UINT8_MAX;
    s_displayed_tlv320_gain_db_x2 = INT8_MAX;
    s_demod_mode = DEMODULATION_MODE_WBFM;
    s_displayed_demod_mode = DEMODULATION_MODE_COUNT;
    s_displayed_active_control = UI_CONTROL_COUNT;
    s_active_control = UI_CONTROL_FREQ_10_MHZ;
    s_volume = UI_VOLUME_DEFAULT;
    s_tlv320_gain_db_x2 = UI_TLV320_GAIN_DEFAULT_DB_X2;
    s_redraw_all = true;
    s_display_mode = UI_DISPLAY_SPLASH;
    s_splash_band_dirty = true;
    s_splash_button_phase = 0U;
    s_displayed_splash_freq_hz = UINT64_MAX;
    s_displayed_splash_appearance = UI_SPLASH_APPEARANCE_COUNT;
    s_splash_spec_poly_valid = false;
    UI_FFT_Init();
    demod_set_mode(s_demod_mode);
    demod_set_gain(ui_volume_gain_q16(s_volume));
    UI_Draw();
}

bool UI_ShouldDrawFft(void)
{
    return (bool)(s_display_mode == UI_DISPLAY_WATERFALL);
}

void UI_Draw(void)
{
    int16_t encoder_delta = encoder_take_delta();
    uint64_t freq_hz = si5351_hw_clk0_get_freq_hz();

    if(encoder_take_button_press())
    {
        ui_handle_button_press();
    }

    ui_sync_display_hw_for_mode();

    if(s_display_mode == UI_DISPLAY_WATERFALL)
    {
        ui_apply_encoder_delta(encoder_delta, freq_hz, &freq_hz);
    }
    else if(encoder_delta != 0)
    {
        uint64_t requested_freq_hz = ui_apply_frequency_delta(freq_hz, encoder_delta, UI_SPLASH_FREQ_STEP_HZ);

        if(usb_hw_set_clk_freq_hz(requested_freq_hz) == READY)
        {
            freq_hz = si5351_hw_clk0_get_freq_hz();
        }
    }

    if(s_display_mode == UI_DISPLAY_WATERFALL)
    {
        if(s_redraw_all ||
            (freq_hz != s_displayed_freq_hz) ||
            (s_volume != s_displayed_volume) ||
            (s_tlv320_gain_db_x2 != s_displayed_tlv320_gain_db_x2) ||
            (s_demod_mode != s_displayed_demod_mode) ||
            (s_active_control != s_displayed_active_control))
        {
            ui_draw_header(freq_hz);
        }
        
        static PeriodicTrigger FFTDraw{1000U / 60U, []() {
            UI_FFT_Compute();
            UI_FFT_Draw();
        }};
        FFTDraw();
    }

    if(s_display_mode == UI_DISPLAY_SPLASH)
    {
        static PeriodicTrigger s_splash_wave_trigger{UI_SPLASH_WAVE_PERIOD_MS, []() {
            ui_draw_splash_waveform();
            ui_draw_splash_spectrum();
        }};

        bool const full_splash = (s_splash_band_dirty || (s_splash_appearance != s_displayed_splash_appearance));

        if(full_splash)
        {
            ui_draw_splash_fullscreen_landscape(freq_hz);
            s_splash_band_dirty = false;
            s_displayed_splash_freq_hz = freq_hz;
            s_displayed_splash_appearance = s_splash_appearance;
            s_splash_wave_trigger.reset();
        }
        else
        {
            if(freq_hz != s_displayed_splash_freq_hz)
            {
                ui_draw_splash_freq_overlay(freq_hz);
                s_displayed_splash_freq_hz = freq_hz;
            }

            s_splash_wave_trigger();
        }
    }
}
