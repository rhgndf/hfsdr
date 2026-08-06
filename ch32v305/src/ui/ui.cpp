#include "ui/ui.h"

extern "C" {
#include "hw/dac.h"
#include "hw/display/splash.h"
#include "hw/encoder.h"
#include "hw/i2s.h"
#include "hw/si5351.h"
#include "hw/tlv320adc6120.h"
}

#include "demod/demod.h"
#include "hw/display/st7789.h"
#include "recording.h"
#include "ui/fft.h"
#include "ui/hw_state.h"
#include "utils/utils.h"

#include "ch32v30x.h"
#include "system_ch32v30x.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>

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
#define UI_REC_TEXT_X      UI_MODE_TEXT_X
#define UI_REC_TEXT_Y      (UI_MODE_TEXT_Y + UI_MODE_TEXT_HEIGHT)
#define UI_REC_TEXT_WIDTH  UI_MODE_TEXT_WIDTH
#define UI_REC_TEXT_HEIGHT Font_11x18.height
#define UI_REC_BLINK_MS    500U
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

/* Match fft.cpp waterfall band (portrait). */
#define UI_WATERFALL_TOP     80U
#define UI_WATERFALL_BOTTOM  320U

/* Landscape splash_freq.png MHz readout quad (clockwise from top-left). */
static constexpr int32_t kSplashTextBaselineDx = 10;
static constexpr int32_t kSplashTextBaselineDy = 1;
static constexpr int32_t kSplashTextShearNumerator = -1;
static constexpr int32_t kSplashTextShearDenominator = 36;
static constexpr uint32_t kSplashFrequencyStepHz = 100000U;

static constexpr size_t kSplashWaveDisplayColumns = 128U;
static constexpr uint32_t kSplashWavePeriodMs = 36U;
static constexpr int32_t kSplashWaveMinimumPeak = 120;
static constexpr int32_t kSplashWavePeakCeiling = 2400;
static constexpr int32_t kSplashWaveDeflectionMultiplier = 1600;
static constexpr int32_t kSplashWaveDeflectionDivisor = 3072;

static constexpr size_t kSplashSpectrumDisplayColumns = 128U;
static constexpr int32_t kSplashSpectrumSampleRateHz = 192000;
static constexpr int32_t kSplashSpectrumMinimumHz = -20000;
static constexpr int32_t kSplashSpectrumMaximumHz = 30000;
static constexpr float kSplashSpectrumMinimumDb = -100.0f;
static constexpr float kSplashSpectrumMaximumDb = 0.0f;

static constexpr uint16_t kSplashTraceXOrigin = 64U;

struct SplashPoint
{
    uint16_t x;
    uint16_t y;
};

using SplashQuad = std::array<SplashPoint, 4U>;

struct SplashTracePoint
{
    uint8_t x_offset;
    uint8_t y;
};

static_assert(sizeof(SplashTracePoint) == 2U);

struct SplashTraces
{
    std::array<SplashTracePoint, kSplashWaveDisplayColumns> wave;
    std::array<SplashTracePoint, kSplashSpectrumDisplayColumns> spectrum;
};

static constexpr SplashQuad kSplashTextQuad{{
    {77U, 24U},
    {167U, 33U},
    {166U, 74U},
    {76U, 60U},
}};

/* Landscape waveform scope quad (clockwise from top-left). */
static constexpr SplashQuad kSplashWaveQuad{{
    {188U, 34U},
    {185U, 102U},
    {314U, 121U},
    {319U, 48U},
}};

static constexpr SplashQuad kSplashSpectrumQuad{{
    {186U, 126U},
    {184U, 176U},
    {314U, 196U},
    {317U, 145U},
}};

enum ui_splash_appearance_t
{
    UI_SPLASH_APPEARANCE_NORMAL,
    UI_SPLASH_APPEARANCE_INVERTED,
    UI_SPLASH_APPEARANCE_COUNT
};

enum ui_display_mode_t
{
    UI_DISPLAY_SPLASH,
    UI_DISPLAY_WATERFALL
};

static ui_display_mode_t s_display_mode = UI_DISPLAY_SPLASH;
static bool s_splash_band_dirty = true;
static ui_splash_appearance_t s_splash_appearance = UI_SPLASH_APPEARANCE_NORMAL;
static ui_splash_appearance_t s_displayed_splash_appearance = UI_SPLASH_APPEARANCE_COUNT;
static uint8_t s_splash_button_phase = 0U;
static uint32_t s_displayed_splash_freq_hz = UINT32_MAX;
static int32_t s_splash_wave_display_peak = kSplashWaveMinimumPeak;
static union
{
    SplashTraces s_splash_traces{};
    std::array<uint8_t, ST7789_BITMAP_BUFFER_BYTES> s_splash_bitmap_buffer;
};
static bool s_splash_wave_poly_valid = false;
static bool s_splash_spec_poly_valid = false;
/* Last MADCTL value applied by UI (0xFF = never synced this session). */
static uint8_t s_hw_madctl = 0xFFU;

static uint32_t s_displayed_freq_hz = UINT32_MAX;
static uint8_t s_displayed_volume = UINT8_MAX;
static int8_t s_displayed_tlv320_gain_db_x2 = INT8_MAX;
static demodulation_mode_t s_demod_mode = DEMODULATION_MODE_WBFM;
static demodulation_mode_t s_displayed_demod_mode = DEMODULATION_MODE_COUNT;
static uint8_t s_volume = UI_VOLUME_DEFAULT;
static bool s_redraw_all = true;
static bool s_displayed_recording = false;
static bool s_recording_blink_inverted = false;

enum ui_control_t
{
    UI_CONTROL_FREQ_10_MHZ,
    UI_CONTROL_FREQ_1_MHZ,
    UI_CONTROL_FREQ_100_KHZ,
    UI_CONTROL_FREQ_10_KHZ,
    UI_CONTROL_FREQ_1_KHZ,
    UI_CONTROL_DEMOD_MODE,
    UI_CONTROL_VOLUME,
    UI_CONTROL_TLV320_GAIN,
    UI_CONTROL_COUNT
};

static ui_control_t s_active_control = UI_CONTROL_FREQ_10_MHZ;
static ui_control_t s_displayed_active_control = UI_CONTROL_COUNT;

static bool ui_control_is_frequency(ui_control_t control)
{
    return control <= UI_CONTROL_FREQ_1_KHZ;
}

static uint32_t ui_frequency_step_hz(ui_control_t control)
{
    switch(control)
    {
        case UI_CONTROL_FREQ_1_KHZ:
            return 1000U;
        case UI_CONTROL_FREQ_10_KHZ:
            return 10000U;
        case UI_CONTROL_FREQ_100_KHZ:
            return 100000U;
        case UI_CONTROL_FREQ_1_MHZ:
            return 1000000U;
        case UI_CONTROL_FREQ_10_MHZ:
            return 10000000U;
        default:
            return 100000U;
    }
}

static uint8_t ui_frequency_digit_power(ui_control_t control)
{
    switch(control)
    {
        case UI_CONTROL_FREQ_1_KHZ:
            return 3U;
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

static uint32_t ui_apply_frequency_delta(uint32_t freq_hz, int16_t delta, uint32_t step_hz)
{
    int64_t const next_freq_hz =
        static_cast<int64_t>(freq_hz) +
        (static_cast<int64_t>(delta) * static_cast<int64_t>(step_hz));

    return static_cast<uint32_t>(
        std::clamp(next_freq_hz,
                   static_cast<int64_t>(SI5351_MIN_OUTPUT_HZ),
                   static_cast<int64_t>(SI5351_MAX_OUTPUT_HZ)));
}

template<typename T>
static T ui_apply_delta(T value, int16_t delta, T minimum, T maximum)
{
    int32_t const next = static_cast<int32_t>(value) + delta;
    return static_cast<T>(std::clamp(next,
                                     static_cast<int32_t>(minimum),
                                     static_cast<int32_t>(maximum)));
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
    s_displayed_splash_freq_hz = UINT32_MAX;
    s_displayed_splash_appearance = UI_SPLASH_APPEARANCE_COUNT;
    s_redraw_all = true;
    s_displayed_active_control = UI_CONTROL_COUNT;
    s_splash_wave_display_peak = kSplashWaveMinimumPeak;
    s_splash_wave_poly_valid = false;
    s_splash_spec_poly_valid = false;
}

extern "C" void ui_handle_button_press(void)
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
        s_displayed_splash_freq_hz = UINT32_MAX;
        s_displayed_splash_appearance = UI_SPLASH_APPEARANCE_COUNT;
        return;
    }

    if(s_active_control == UI_CONTROL_TLV320_GAIN)
    {
        s_display_mode = UI_DISPLAY_SPLASH;
        s_splash_band_dirty = true;
        s_splash_button_phase = 0U;
        s_displayed_splash_freq_hz = UINT32_MAX;
        s_displayed_splash_appearance = UI_SPLASH_APPEARANCE_COUNT;
        return;
    }

    s_active_control = (ui_control_t)((uint32_t)s_active_control + 1U);
}

extern "C" void ui_handle_button_long_press(void)
{
    recording_request_toggle_from_isr();
}

static void ui_draw_selected_frequency_digit(void)
{
    uint8_t digit_power = ui_frequency_digit_power(s_active_control);
    uint16_t digit_index = (uint16_t)(UI_FREQ_DIGITS - 1U - digit_power);

    uint16_t center_x = (uint16_t)((digit_index * Font_16x26.width) + (Font_16x26.width / 2U));
    ST7789_DrawFilledTriangle((uint16_t)(center_x - 5U), UI_TRIANGLE_Y,
                              (uint16_t)(center_x + 5U), UI_TRIANGLE_Y,
                              center_x, (uint16_t)(UI_TRIANGLE_Y + 7U),
                              YELLOW);
}

static void ui_draw_frequency(uint32_t freq_hz)
{
    char freq_text[24];

    snprintf(freq_text, sizeof(freq_text), "%9lu", (unsigned long)freq_hz);

    ST7789_Fill(0U, 0U, (uint16_t)(UI_FREQ_TEXT_WIDTH - 1U), (uint16_t)(UI_FREQ_TEXT_Y - 1U), BLACK);
    ST7789_WriteString(0U, UI_FREQ_TEXT_Y, freq_text, Font_16x26, WHITE, BLACK);
    ST7789_WriteString(UI_FREQ_SUFFIX_X, UI_FREQ_TEXT_Y, "Hz", Font_16x26, WHITE, BLACK);
    if(ui_control_is_frequency(s_active_control))
    {
        ui_draw_selected_frequency_digit();
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

static void ui_draw_recording_indicator(bool recording)
{
    if(!recording)
    {
        ST7789_Fill(UI_REC_TEXT_X,
                    UI_REC_TEXT_Y,
                    (uint16_t)(UI_REC_TEXT_X + UI_REC_TEXT_WIDTH - 1U),
                    (uint16_t)(UI_REC_TEXT_Y + UI_REC_TEXT_HEIGHT - 1U),
                    BLACK);
    }
    else
    {
        uint16_t const fg = s_recording_blink_inverted ? BLACK : RED;
        uint16_t const bg = s_recording_blink_inverted ? RED : BLACK;
        ST7789_WriteString(UI_REC_TEXT_X, UI_REC_TEXT_Y, "REC ", Font_11x18, fg, bg);
    }

    s_displayed_recording = recording;
}

static void ui_update_recording_indicator(void)
{
    bool const recording = recording_is_active();

    if(recording != s_displayed_recording)
    {
        s_recording_blink_inverted = false;
        ui_draw_recording_indicator(recording);
    }
}

static void ui_draw_progress_row(uint16_t y,
                                 char const *label,
                                 int16_t value,
                                 int16_t minimum,
                                 int16_t maximum,
                                 bool active)
{
    uint16_t color = active ? YELLOW : WHITE;
    uint16_t fill_color = active ? YELLOW : GREEN;
    uint16_t bar_y = (uint16_t)(y + 3U);
    uint16_t inner_width = UI_BAR_WIDTH - 2U;
    uint16_t filled_width = 0U;

    if(maximum > minimum)
    {
        int16_t const clamped = std::clamp(value, minimum, maximum);
        filled_width = (uint16_t)(((uint32_t)inner_width * (uint32_t)(clamped - minimum)) /
                                  (uint32_t)(maximum - minimum));
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

static void ui_draw_header(uint32_t freq_hz, int8_t gain_db_x2)
{
    if(s_redraw_all)
    {
        ST7789_Fill(0U, 0U, ST7789_WIDTH - 1U, UI_HEADER_BOTTOM_Y, BLACK);
    }

    ui_draw_mode_control();
    bool const recording = recording_is_active();
    if(recording != s_displayed_recording)
    {
        s_recording_blink_inverted = false;
    }
    ui_draw_recording_indicator(recording);
    ui_draw_frequency(freq_hz);
    ui_draw_progress_row(UI_VOL_ROW_Y,
                         "VOL",
                         s_volume,
                         0,
                         UI_VOLUME_MAX,
                         s_active_control == UI_CONTROL_VOLUME);
    ui_draw_progress_row(UI_GAIN_ROW_Y,
                         "GAIN",
                         gain_db_x2,
                         UI_TLV320_GAIN_MIN_DB_X2,
                         UI_TLV320_GAIN_MAX_DB_X2,
                         s_active_control == UI_CONTROL_TLV320_GAIN);

    s_displayed_freq_hz = freq_hz;
    s_displayed_volume = s_volume;
    s_displayed_tlv320_gain_db_x2 = gain_db_x2;
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

static std::pair<uint16_t, uint16_t> ui_splash_colors(void)
{
    if(s_splash_appearance == UI_SPLASH_APPEARANCE_INVERTED)
    {
        return {BLACK, WHITE};
    }

    return {WHITE, BLACK};
}

static std::optional<int32_t> ui_splash_edge_x_at_y(SplashPoint first,
                                                    SplashPoint second,
                                                    int32_t y)
{
    int32_t const x0 = static_cast<int32_t>(first.x);
    int32_t const y0 = static_cast<int32_t>(first.y);
    int32_t const x1 = static_cast<int32_t>(second.x);
    int32_t const y1 = static_cast<int32_t>(second.y);

    if(y0 == y1)
    {
        return (y == y0) ? std::optional<int32_t>{x0} : std::nullopt;
    }

    auto const [y_min, y_max] = std::minmax(y0, y1);
    if((y < y_min) || (y > y_max))
    {
        return std::nullopt;
    }

    return x0 + ((y - y0) * (x1 - x0)) / (y1 - y0);
}

static std::optional<std::pair<int32_t, int32_t>>
ui_splash_quad_scanline_bounds(SplashQuad const &quad, int32_t y)
{
    std::array<int32_t, 4U> intersections{};
    size_t count = 0U;

    for(size_t i = 0U; i < quad.size(); ++i)
    {
        std::optional<int32_t> const x =
            ui_splash_edge_x_at_y(quad[i], quad[(i + 1U) % quad.size()], y);
        if(x)
        {
            intersections[count++] = *x;
        }
    }

    if(count < 2U)
    {
        return std::nullopt;
    }

    auto const [minimum, maximum] =
        std::minmax_element(intersections.begin(), intersections.begin() + count);
    return std::pair{*minimum, *maximum};
}

static std::pair<uint16_t, uint16_t> ui_splash_quad_y_bounds(SplashQuad const &quad)
{
    auto const [minimum, maximum] =
        std::minmax_element(quad.begin(),
                            quad.end(),
                            [](SplashPoint const &lhs, SplashPoint const &rhs) {
                                return lhs.y < rhs.y;
                            });
    return {minimum->y, maximum->y};
}

static void ui_splash_fill_quad(SplashQuad const &quad, uint16_t color)
{
    auto const [y_min, y_max] = ui_splash_quad_y_bounds(quad);

    for(int32_t y = static_cast<int32_t>(y_min);
        y <= static_cast<int32_t>(y_max);
        ++y)
    {
        std::optional<std::pair<int32_t, int32_t>> const bounds =
            ui_splash_quad_scanline_bounds(quad, y);
        if(!bounds)
        {
            continue;
        }

        auto [x_lo, x_hi] = *bounds;
        x_lo = std::max<int32_t>(x_lo, 0);

        ST7789_Fill(static_cast<uint16_t>(x_lo),
                    static_cast<uint16_t>(y),
                    static_cast<uint16_t>(x_hi),
                    static_cast<uint16_t>(y),
                    color);
    }
}

static uint16_t ui_splash_clamp_u16(int32_t v, uint16_t max_u)
{
    return static_cast<uint16_t>(
        std::clamp<int32_t>(v, 0, max_u));
}

static SplashTracePoint ui_splash_trace_point(uint16_t x, uint16_t y)
{
    int32_t const x_offset =
        std::clamp<int32_t>(static_cast<int32_t>(x) - kSplashTraceXOrigin,
                            0,
                            UINT8_MAX);
    uint16_t const clamped_y =
        std::min(y, static_cast<uint16_t>(UINT8_MAX));

    return {
        .x_offset = static_cast<uint8_t>(x_offset),
        .y = static_cast<uint8_t>(clamped_y),
    };
}

static uint16_t ui_splash_trace_x(SplashTracePoint point)
{
    return static_cast<uint16_t>(kSplashTraceXOrigin + point.x_offset);
}

template<size_t N>
static void ui_splash_erase_trace(std::array<SplashTracePoint, N> const &trace,
                                  uint16_t color)
{
    for(size_t i = 1U; i < trace.size(); ++i)
    {
        ST7789_DrawLineFills(ui_splash_trace_x(trace[i - 1U]),
                             trace[i - 1U].y,
                             ui_splash_trace_x(trace[i]),
                             trace[i].y,
                             color);
    }
}

static int32_t ui_waveform_get(size_t idx)
{
    const volatile uint32_t* buf = dac_hw_stream_ring_samples();
    const volatile uint16_t* buf_idx = (const volatile uint16_t*)&buf[idx];
    return *buf_idx;
}

static void ui_draw_splash_waveform(void)
{
    auto const [scope_fill, trace_color] = ui_splash_colors();
    size_t const display_cols = s_splash_traces.wave.size();
    size_t const n = DAC_HW_STREAM_RING_SAMPLES;
    if((n < 2U) || (display_cols < 2U))
    {
        s_splash_wave_poly_valid = false;
        return;
    }

    int32_t frame_peak = kSplashWaveMinimumPeak;
    for(size_t i = 0U; i < n; ++i)
    {
        int32_t const raw = static_cast<int32_t>(ui_waveform_get(i)) - 2048;
        frame_peak = std::max(frame_peak, std::abs(raw));
    }

    if(frame_peak > s_splash_wave_display_peak)
    {
        s_splash_wave_display_peak = frame_peak;
    }
    else
    {
        int32_t const decay =
            std::max<int32_t>(s_splash_wave_display_peak / 24, 6);
        s_splash_wave_display_peak =
            std::max(s_splash_wave_display_peak - decay, kSplashWaveMinimumPeak);
    }

    s_splash_wave_display_peak =
        std::min(s_splash_wave_display_peak, kSplashWavePeakCeiling);

    uint16_t const w_lim = static_cast<uint16_t>(ST7789_GetWidth() - 1U);
    uint16_t const h_lim = static_cast<uint16_t>(ST7789_GetHeight() - 1U);

    int32_t const den_plot = static_cast<int32_t>(display_cols - 1U);
    int32_t const dx_top =
        static_cast<int32_t>(kSplashWaveQuad[3U].x) - kSplashWaveQuad[0U].x;
    int32_t const dy_top =
        static_cast<int32_t>(kSplashWaveQuad[3U].y) - kSplashWaveQuad[0U].y;
    int32_t const dx_bottom =
        static_cast<int32_t>(kSplashWaveQuad[2U].x) - kSplashWaveQuad[1U].x;
    int32_t const dy_bottom =
        static_cast<int32_t>(kSplashWaveQuad[2U].y) - kSplashWaveQuad[1U].y;

    if(s_splash_wave_poly_valid)
    {
        ui_splash_erase_trace(s_splash_traces.wave, scope_fill);
    }
    else
    {
        ui_splash_fill_quad(kSplashWaveQuad, scope_fill);
    }

    uint16_t prev_px = 0U;
    uint16_t prev_py = 0U;
    for(size_t i = 0U; i < display_cols; ++i)
    {
        size_t const j = (i * (n - 1U)) / static_cast<size_t>(den_plot);
        size_t const jm = (j > 0U) ? j - 1U : 0U;
        size_t const jp = std::min(j + 1U, n - 1U);
        int32_t const sum3 =
            ui_waveform_get(jm) + ui_waveform_get(j) + ui_waveform_get(jp);
        int32_t const raw = (sum3 / 3) - 2048;
        int32_t const amplitude =
            (raw * kSplashWaveDeflectionMultiplier) / s_splash_wave_display_peak;

        int32_t const x_top =
            kSplashWaveQuad[0U].x + ((static_cast<int32_t>(i) * dx_top) / den_plot);
        int32_t const y_top =
            kSplashWaveQuad[0U].y + ((static_cast<int32_t>(i) * dy_top) / den_plot);
        int32_t const x_bottom =
            kSplashWaveQuad[1U].x + ((static_cast<int32_t>(i) * dx_bottom) / den_plot);
        int32_t const y_bottom =
            kSplashWaveQuad[1U].y + ((static_cast<int32_t>(i) * dy_bottom) / den_plot);
        int32_t const x_center = (x_top + x_bottom) / 2;
        int32_t const y_center = (y_top + y_bottom) / 2;
        int32_t const vx = x_bottom - x_top;
        int32_t const vy = y_bottom - y_top;

        int32_t const px =
            x_center + ((vx * amplitude) / kSplashWaveDeflectionDivisor);
        int32_t const py =
            y_center + ((vy * amplitude) / kSplashWaveDeflectionDivisor);

        uint16_t const curr_px = ui_splash_clamp_u16(px, w_lim);
        uint16_t const curr_py = ui_splash_clamp_u16(py, h_lim);
        if(i > 0U)
        {
            ST7789_DrawLineFills(prev_px, prev_py, curr_px, curr_py, trace_color);
        }
        s_splash_traces.wave[i] = ui_splash_trace_point(curr_px, curr_py);
        prev_px = curr_px;
        prev_py = curr_py;
    }

    s_splash_wave_poly_valid = true;
}

static float ui_splash_spec_fft_bin_at_hz(int32_t hz, uint32_t bin_count)
{
    return ((static_cast<float>(hz) +
             (static_cast<float>(kSplashSpectrumSampleRateHz) * 0.5f)) *
            static_cast<float>(bin_count)) /
           static_cast<float>(kSplashSpectrumSampleRateHz);
}

static float ui_splash_spec_db_at_bin(const float *fft_buf, uint32_t bin_count, float bin)
{
    float const max_bin = static_cast<float>(bin_count - 2U);
    bin = std::clamp(bin, 0.0f, max_bin);

    uint32_t const idx = static_cast<uint32_t>(bin);
    float const frac = bin - static_cast<float>(idx);

    return fft_buf[idx] + (frac * (fft_buf[idx + 1U] - fft_buf[idx]));
}

static uint16_t ui_splash_spec_y_frac(float db)
{
    float const normalized =
        std::clamp((db - kSplashSpectrumMinimumDb) /
                       (kSplashSpectrumMaximumDb - kSplashSpectrumMinimumDb),
                   0.0f,
                   1.0f);

    return static_cast<uint16_t>(normalized * 1024.0f);
}

static void ui_draw_splash_spectrum(void)
{
    auto const [scope_fill, trace_color] = ui_splash_colors();

    if(!s_splash_spec_poly_valid)
    {
        ui_splash_fill_quad(kSplashSpectrumQuad, scope_fill);
    }

    UI_FFT_Compute();

    float const *fft_buf = UI_FFT_Buffer();
    uint32_t const bin_count = UI_FFT_BinCount();
    if((fft_buf == nullptr) || (bin_count < 2U))
    {
        s_splash_spec_poly_valid = false;
        return;
    }

    size_t const display_cols = s_splash_traces.spectrum.size();
    if(s_splash_spec_poly_valid)
    {
        ui_splash_erase_trace(s_splash_traces.spectrum, scope_fill);
    }

    uint16_t const w_lim = static_cast<uint16_t>(ST7789_GetWidth() - 1U);
    uint16_t const h_lim = static_cast<uint16_t>(ST7789_GetHeight() - 1U);
    int32_t const den_plot = static_cast<int32_t>(display_cols - 1U);
    int32_t const dx_top =
        static_cast<int32_t>(kSplashSpectrumQuad[3U].x) - kSplashSpectrumQuad[0U].x;
    int32_t const dy_top =
        static_cast<int32_t>(kSplashSpectrumQuad[3U].y) - kSplashSpectrumQuad[0U].y;
    int32_t const dx_bottom =
        static_cast<int32_t>(kSplashSpectrumQuad[2U].x) - kSplashSpectrumQuad[1U].x;
    int32_t const dy_bottom =
        static_cast<int32_t>(kSplashSpectrumQuad[2U].y) - kSplashSpectrumQuad[1U].y;
    float const bin_min =
        ui_splash_spec_fft_bin_at_hz(kSplashSpectrumMinimumHz, bin_count);
    float const bin_max =
        ui_splash_spec_fft_bin_at_hz(kSplashSpectrumMaximumHz, bin_count);
    float const bin_step =
        (bin_max - bin_min) / static_cast<float>(den_plot);
    float bin = bin_min;

    uint16_t prev_px = 0U;
    uint16_t prev_py = 0U;
    for(size_t i = 0U; i < display_cols; ++i)
    {
        int32_t const x_top =
            kSplashSpectrumQuad[0U].x + ((static_cast<int32_t>(i) * dx_top) / den_plot);
        int32_t const y_top =
            kSplashSpectrumQuad[0U].y + ((static_cast<int32_t>(i) * dy_top) / den_plot);
        int32_t const x_bottom =
            kSplashSpectrumQuad[1U].x +
            ((static_cast<int32_t>(i) * dx_bottom) / den_plot);
        int32_t const y_bottom =
            kSplashSpectrumQuad[1U].y +
            ((static_cast<int32_t>(i) * dy_bottom) / den_plot);
        uint16_t const y_fraction =
            ui_splash_spec_y_frac(ui_splash_spec_db_at_bin(fft_buf, bin_count, bin));
        int32_t const px =
            x_bottom + (((x_top - x_bottom) * static_cast<int32_t>(y_fraction)) / 1024);
        int32_t const py =
            y_bottom + (((y_top - y_bottom) * static_cast<int32_t>(y_fraction)) / 1024);
        uint16_t const curr_px = ui_splash_clamp_u16(px, w_lim);
        uint16_t const curr_py = ui_splash_clamp_u16(py, h_lim);

        if(i > 0U)
        {
            ST7789_DrawLineFills(prev_px, prev_py, curr_px, curr_py, trace_color);
        }

        s_splash_traces.spectrum[i] = ui_splash_trace_point(curr_px, curr_py);
        prev_px = curr_px;
        prev_py = curr_py;
        bin += bin_step;
    }

    s_splash_spec_poly_valid = true;
    i2s_fft_sample_arr_reset();
}

static void ui_draw_splash_freq_overlay(uint32_t freq_hz)
{
    char text[20];
    if(freq_hz >= 1000000U)
    {
        unsigned long const mhz = static_cast<unsigned long>(freq_hz / 1000000U);
        unsigned long const mhz_frac =
            static_cast<unsigned long>((freq_hz % 1000000U) / 100000U);
        std::snprintf(text, sizeof(text), "%lu.%lu MHz", mhz, mhz_frac);
    }
    else if(freq_hz >= 1000U)
    {
        unsigned long const khz = static_cast<unsigned long>(freq_hz / 1000U);
        unsigned long const khz_frac =
            static_cast<unsigned long>((freq_hz % 1000U) / 100U);
        std::snprintf(text, sizeof(text), "%lu.%lu kHz", khz, khz_frac);
    }
    else
    {
        std::snprintf(text,
                      sizeof(text),
                      "%lu Hz",
                      static_cast<unsigned long>(freq_hz));
    }

    auto const [text_fg, text_bg] = ui_splash_colors();
    ui_splash_fill_quad(kSplashTextQuad, text_bg);

    size_t const text_len = std::strlen(text);
    int32_t const top_span_x =
        static_cast<int32_t>(kSplashTextQuad[1U].x) - kSplashTextQuad[0U].x;
    int32_t const text_span_x =
        static_cast<int32_t>(text_len) * kSplashTextBaselineDx;
    int32_t const horizontal_offset = (top_span_x - text_span_x) / 2;
    int32_t const start_x = kSplashTextQuad[0U].x + horizontal_offset;
    int32_t start_y = kSplashTextQuad[0U].y + 12;

    if(top_span_x > 0)
    {
        int32_t const top_span_y =
            static_cast<int32_t>(kSplashTextQuad[1U].y) - kSplashTextQuad[0U].y;
        start_y += (horizontal_offset * top_span_y) / top_span_x;
    }

    ST7789_WriteStringSlanted(start_x,
                              start_y,
                              text,
                              Font_11x18,
                              text_fg,
                              kSplashTextBaselineDx,
                              kSplashTextBaselineDy,
                              kSplashTextShearNumerator,
                              kSplashTextShearDenominator);
}

static void ui_draw_splash_fullscreen_landscape(uint32_t freq_hz)
{
    s_splash_wave_poly_valid = false;
    s_splash_spec_poly_valid = false;

    auto const [bitmap_fg, bitmap_bg] = ui_splash_colors();

    ST7789_Fill_Color(bitmap_bg);
    std::construct_at(&s_splash_bitmap_buffer);
    ST7789_DrawBitmap1bpp(0U,
                          0U,
                          splash_freq_screen_w,
                          splash_freq_screen_h,
                          splash_freq_screen,
                          bitmap_fg,
                          bitmap_bg,
                          s_splash_bitmap_buffer.data(),
                          s_splash_bitmap_buffer.size());
    std::construct_at(&s_splash_traces);
    ui_draw_splash_freq_overlay(freq_hz);
    ui_draw_splash_waveform();
    ui_draw_splash_spectrum();
}

static uint32_t ui_apply_encoder_delta(int16_t delta, uint32_t freq_hz)
{
    if(delta == 0)
    {
        return freq_hz;
    }

    switch(s_active_control)
    {
        case UI_CONTROL_FREQ_10_KHZ:
        case UI_CONTROL_FREQ_1_KHZ:
        case UI_CONTROL_FREQ_100_KHZ:
        case UI_CONTROL_FREQ_1_MHZ:
        case UI_CONTROL_FREQ_10_MHZ:
        {
            uint32_t const requested_frequency = hw_state_get_requested_frequency();
            uint32_t requested_freq_hz =
                ui_apply_frequency_delta(requested_frequency,
                                         delta,
                                         ui_frequency_step_hz(s_active_control));

            if(requested_freq_hz != requested_frequency)
            {
                hw_state_set_frequency(requested_freq_hz);
            }
            break;
        }

        case UI_CONTROL_VOLUME:
        {
            uint8_t const volume =
                ui_apply_delta(s_volume,
                               delta,
                               uint8_t{0U},
                               static_cast<uint8_t>(UI_VOLUME_MAX));

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
                s_demod_mode = (s_demod_mode == static_cast<demodulation_mode_t>(DEMODULATION_MODE_COUNT - 1))
                    ? DEMODULATION_MODE_WBFM
                    : static_cast<demodulation_mode_t>(s_demod_mode + 1);
            }
            else
            {
                s_demod_mode = (s_demod_mode == DEMODULATION_MODE_WBFM)
                    ? static_cast<demodulation_mode_t>(DEMODULATION_MODE_COUNT - 1)
                    : static_cast<demodulation_mode_t>(s_demod_mode - 1);
            }

            demod_set_mode(s_demod_mode);
            break;
        }

        case UI_CONTROL_TLV320_GAIN:
        {
            int8_t const requested_gain = hw_state_get_gain_x2();
            int8_t const gain_db_x2 =
                ui_apply_delta(requested_gain,
                               delta,
                               static_cast<int8_t>(UI_TLV320_GAIN_MIN_DB_X2),
                               static_cast<int8_t>(UI_TLV320_GAIN_MAX_DB_X2));

            if(gain_db_x2 != requested_gain)
            {
                hw_state_set_gain_x2(gain_db_x2);
            }
            break;
        }

        case UI_CONTROL_COUNT:
        default:
            break;
    }

    return freq_hz;
}

void UI_Init(void)
{
    s_displayed_freq_hz = UINT32_MAX;
    s_displayed_volume = UINT8_MAX;
    s_displayed_tlv320_gain_db_x2 = INT8_MAX;
    s_demod_mode = DEMODULATION_MODE_WBFM;
    s_displayed_demod_mode = DEMODULATION_MODE_COUNT;
    s_displayed_active_control = UI_CONTROL_COUNT;
    s_active_control = UI_CONTROL_FREQ_10_MHZ;
    s_volume = UI_VOLUME_DEFAULT;
    s_redraw_all = true;
    s_displayed_recording = false;
    s_recording_blink_inverted = false;
    s_display_mode = UI_DISPLAY_SPLASH;
    s_splash_band_dirty = true;
    s_splash_button_phase = 0U;
    s_displayed_splash_freq_hz = UINT32_MAX;
    s_displayed_splash_appearance = UI_SPLASH_APPEARANCE_COUNT;
    s_splash_spec_poly_valid = false;
    UI_FFT_Init();
    demod_set_mode(s_demod_mode);
    demod_set_gain(ui_volume_gain_q16(s_volume));
    UI_Draw();
}

bool UI_ShouldDrawFft(void)
{
    return s_display_mode == UI_DISPLAY_WATERFALL;
}

void UI_Draw(void)
{
    int16_t encoder_delta = encoder_take_delta();
    uint32_t freq_hz = hw_state_get_frequency();
    int8_t gain_db_x2 = hw_state_get_gain_x2();

    ui_sync_display_hw_for_mode();

    if(s_display_mode == UI_DISPLAY_WATERFALL)
    {
        freq_hz = ui_apply_encoder_delta(encoder_delta, freq_hz);
    }
    else if(encoder_delta != 0)
    {
        uint32_t requested_freq_hz =
            ui_apply_frequency_delta(hw_state_get_requested_frequency(),
                                     encoder_delta,
                                     kSplashFrequencyStepHz);

        if(requested_freq_hz != hw_state_get_requested_frequency())
        {
            hw_state_set_frequency(requested_freq_hz);
        }
    }

    if(s_display_mode == UI_DISPLAY_WATERFALL)
    {
        if(s_redraw_all ||
            (freq_hz != s_displayed_freq_hz) ||
            (s_volume != s_displayed_volume) ||
            (gain_db_x2 != s_displayed_tlv320_gain_db_x2) ||
            (s_demod_mode != s_displayed_demod_mode) ||
            (s_active_control != s_displayed_active_control))
        {
            ui_draw_header(freq_hz, gain_db_x2);
        }

        ui_update_recording_indicator();

        static PeriodicTrigger RecordingBlink{UI_REC_BLINK_MS, []() {
            if(!recording_is_active())
            {
                return;
            }

            s_recording_blink_inverted = !s_recording_blink_inverted;
            ui_draw_recording_indicator(true);
        }};
        RecordingBlink();
        
        static PeriodicTrigger FFTDraw{1000U / 60U, []() {
            UI_FFT_Compute();
            UI_FFT_Draw();
        }};
        FFTDraw();
    }

    if(s_display_mode == UI_DISPLAY_SPLASH)
    {
        static PeriodicTrigger s_splash_wave_trigger{kSplashWavePeriodMs, []() {
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
