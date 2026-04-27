#include "ui/ui.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "feature/fm_audio_out/fm_audio_out.h"
#include "hw/display/st7789.h"
#include "hw/encoder.h"
#include "hw/si5351.h"
#include "hw/tlv320adc6120.h"
#include "hw/usb.h"

#define UI_HEADER_BOTTOM_Y 79U
#define UI_FREQ_TEXT_Y     10U
#define UI_FREQ_DIGITS     9U
#define UI_FREQ_SUFFIX_CHARS 3U
#define UI_FREQ_TEXT_WIDTH ((UI_FREQ_DIGITS + UI_FREQ_SUFFIX_CHARS) * Font_16x26.width)
#define UI_FREQ_SUFFIX_X   ((UI_FREQ_DIGITS * Font_16x26.width) + (Font_16x26.width / 2U))
#define UI_MODE_TEXT_X     (ST7789_WIDTH - (4U * Font_11x18.width) - 1U)
#define UI_MODE_TEXT_Y     0U
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

static uint64_t s_displayed_freq_hz = UINT64_MAX;
static uint8_t s_displayed_volume = UINT8_MAX;
static int8_t s_displayed_tlv320_gain_db_x2 = INT8_MAX;
static uint8_t s_volume = UI_VOLUME_DEFAULT;
static int8_t s_tlv320_gain_db_x2 = UI_TLV320_GAIN_DEFAULT_DB_X2;
static bool s_redraw_all = true;

typedef enum
{
    UI_CONTROL_FREQ_10_MHZ,
    UI_CONTROL_FREQ_1_MHZ,
    UI_CONTROL_FREQ_100_KHZ,
    UI_CONTROL_FREQ_10_KHZ,
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

static void ui_cycle_active_control(void)
{
    s_active_control = (ui_control_t)(((uint32_t)s_active_control + 1U) % (uint32_t)UI_CONTROL_COUNT);
}

static void ui_draw_selected_frequency_digit(uint64_t freq_hz)
{
    uint8_t digit_power = ui_frequency_digit_power(s_active_control);
    uint16_t digit_index = (uint16_t)(UI_FREQ_DIGITS - 1U - digit_power);
    uint16_t center_x;

    (void)freq_hz;

    center_x = (uint16_t)((digit_index * Font_16x26.width) + (Font_16x26.width / 2U));
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
        ST7789_WriteString(UI_MODE_TEXT_X, UI_MODE_TEXT_Y, "WBFM", Font_11x18, CYAN, BLACK);
    }

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
    s_displayed_active_control = s_active_control;
    s_redraw_all = false;
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
                fm_audio_out_set_gain(ui_volume_gain_q16(s_volume));
            }
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
    s_displayed_active_control = UI_CONTROL_COUNT;
    s_active_control = UI_CONTROL_FREQ_10_MHZ;
    s_volume = UI_VOLUME_DEFAULT;
    s_tlv320_gain_db_x2 = UI_TLV320_GAIN_DEFAULT_DB_X2;
    s_redraw_all = true;
    fm_audio_out_set_gain(ui_volume_gain_q16(s_volume));
    UI_Draw();
}

void UI_Draw(void)
{
    int16_t encoder_delta = encoder_take_delta();
    uint64_t freq_hz = si5351_hw_clk0_get_freq_hz();

    if(encoder_take_button_press())
    {
        ui_cycle_active_control();
    }

    ui_apply_encoder_delta(encoder_delta, freq_hz, &freq_hz);

    if(s_redraw_all ||
       (freq_hz != s_displayed_freq_hz) ||
       (s_volume != s_displayed_volume) ||
       (s_tlv320_gain_db_x2 != s_displayed_tlv320_gain_db_x2) ||
       (s_active_control != s_displayed_active_control))
    {
        ui_draw_header(freq_hz);
    }
}
