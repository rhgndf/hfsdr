#include "ui/ui.h"

#include <stdint.h>
#include <stdio.h>

#include "hw/display/st7789.h"
#include "hw/encoder.h"
#include "hw/si5351.h"
#include "hw/usb.h"

#define UI_FREQ_STEP_HZ 100000LL

static uint64_t s_displayed_freq_hz = UINT64_MAX;

static uint64_t ui_apply_encoder_delta(uint64_t freq_hz, int16_t delta)
{
    int64_t next_freq_hz = (int64_t)freq_hz + ((int64_t)delta * UI_FREQ_STEP_HZ);

    if(next_freq_hz < (int64_t)SI5351_MIN_OUTPUT_HZ)
    {
        next_freq_hz = (int64_t)SI5351_MIN_OUTPUT_HZ;
    }

    return (uint64_t)next_freq_hz;
}

static void ui_draw_frequency(uint64_t freq_hz)
{
    char freq_text[24];
    uint32_t mhz = (uint32_t)(freq_hz / 1000000ULL);
    uint32_t khz = (uint32_t)((freq_hz % 1000000ULL) / 1000ULL);

    snprintf(freq_text, sizeof(freq_text), "%lu.%03lu MHz",
             (unsigned long)mhz,
             (unsigned long)khz);

    ST7789_Fill(0U, 0U, ST7789_WIDTH - 1U, Font_16x26.height - 1U, BLACK);
    ST7789_WriteString(0U, 0U, freq_text, Font_16x26, WHITE, BLACK);
}

void UI_Init(void)
{
    s_displayed_freq_hz = UINT64_MAX;
    UI_Draw();
}

void UI_Draw(void)
{
    int16_t encoder_delta = encoder_take_delta();
    uint64_t freq_hz = si5351_hw_clk0_get_freq_hz();

    if(encoder_delta != 0)
    {
        uint64_t next_freq_hz = ui_apply_encoder_delta(freq_hz, encoder_delta);

        if(usb_hw_set_clk_freq_hz(next_freq_hz) == READY)
        {
            freq_hz = si5351_hw_clk0_get_freq_hz();
        }
    }

    if(freq_hz != s_displayed_freq_hz)
    {
        ui_draw_frequency(freq_hz);
        s_displayed_freq_hz = freq_hz;
    }
}
