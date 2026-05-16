/********************************** (C) COPYRIGHT *******************************
* File Name          : main.c
* Author             : WCH
* Version            : V1.0.0
* Date               : 2021/06/06
* Description        : Main program body.
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for 
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/

/*
 *@Note
 GPIO routine:
 PA0 push-pull output.

*/


#include "debug.h"
#include <stddef.h>

extern "C" {
#include "hw/pinout.h"
#include "hw/dac.h"
#include "hw/encoder.h"
#include "hw/display/st7789.h"
#include "hw/display/cst328.h"

#include "hw/i2c.h"
#include "hw/tlv320adc6120.h"
#include "hw/si5351.h"
#include "hw/i2s.h"
#include "hw/usb.h"
#include "hw/watchdog.h"
#include "hw/adc.h"
#include "feature/blinky/blinky.h"
#include "feature/fm_audio_out/fm_audio_out.h"
#include "ui/ui.h"
}

#include "feature/iq_calibration/iq_calibration.h"
#include "hw/sdcard/sdcard.h"
#include "utils/utils.h"
#include "tusb.h"


constexpr uint64_t InitialCalibrationFreq = 144020000ULL;
constexpr uint64_t InitialFMFreq = 92400000ULL;
static void Draw_I2S_Sync_Status(void);
static void Boot_Display_InitLandscape(void);

static void SysTick_Report_USB_EverySecond(void)
{
    static uint64_t last_report_tick = 0;
    uint64_t now_tick = SysTick->CNT;

    printf("%d systick_now=0x%lx last=0x%lx %d\r\n",
           25,
           (uint32_t)now_tick,
           (uint32_t)last_report_tick,
           25);

    last_report_tick = now_tick;
}

static void Scan_I2CBus_EverySecond(void)
{
    printf("I2C scan:");

    uint8_t device_count = 0;
    for(uint8_t addr = 0x08U; addr <= 0x77U; ++addr)
    {
        if(i2c_hw_scan_bus_at(addr) == READY)
        {
            ++device_count;
            printf(" 0x%02X", addr);
        }
    }

    if(device_count == 0U)
    {
        printf(" no devices\r\n");
    }
    else
    {
        printf(" (%u)\r\n", device_count);
    }
}

static uint8_t s_tlv320_i2s_report_initialized = 0U;

static void TLV320_I2S_Poll(void)
{
    static uint32_t last_word_count = 0U;
    static uint32_t last_vendor_total_word_count = 0U;
    static uint32_t last_vendor_dropped_word_count = 0U;
    uint32_t words_now = i2s_hw_rx_word_count();
    uint32_t vendor_words_now = usb_hw_vendor_total_words();
    uint32_t vendor_dropped_words_now = usb_hw_vendor_dropped_words();

    if((s_tlv320_i2s_report_initialized == 0U) ||
       (words_now < last_word_count) ||
       (vendor_words_now < last_vendor_total_word_count) ||
       (vendor_dropped_words_now < last_vendor_dropped_word_count))
    {
        last_word_count = words_now;
        last_vendor_total_word_count = vendor_words_now;
        last_vendor_dropped_word_count = vendor_dropped_words_now;
        s_tlv320_i2s_report_initialized = 1U;
    }
    else
    {
        uint32_t words_per_sec = words_now - last_word_count;
        uint32_t frames_per_sec = words_per_sec / 4U;
        uint32_t bytes_per_sec = words_per_sec * (uint32_t)sizeof(uint16_t);
        uint32_t vendor_words_per_sec = vendor_words_now - last_vendor_total_word_count;
        uint32_t vendor_dropped_words_per_sec = vendor_dropped_words_now - last_vendor_dropped_word_count;

        printf("ADC I2S rate: %lu words/s, %lu frames/s, %lu B/s | vendor %lu words/s drop %lu words/s | LO %lu Hz\r\n",
               (unsigned long)words_per_sec,
               (unsigned long)frames_per_sec,
               (unsigned long)bytes_per_sec,
               (unsigned long)vendor_words_per_sec,
               (unsigned long)vendor_dropped_words_per_sec,
               (unsigned long)si5351_hw_clk0_get_freq_hz());

        last_word_count = words_now;
        last_vendor_total_word_count = vendor_words_now;
        last_vendor_dropped_word_count = vendor_dropped_words_now;
    }
}

static uint8_t s_i2s_bitslip_check = 1U;
static uint8_t s_i2s_bitslip_false_checks_in_a_row = 0U;
static uint8_t s_i2s_bitslip_sync_displayed_count = UINT8_MAX;

static void Boot_Display_InitLandscape(void)
{
    ST7789_SetRotation(3U);
    ST7789_VerticalScrollDisable();
    ST7789_Fill_Color(BLACK);
}

static uint16_t I2S_Sync_Bar_X(uint32_t value, uint32_t samples)
{
    constexpr uint16_t bar_x = 18U;
    const uint16_t bar_width = (uint16_t)(ST7789_GetWidth() - (bar_x * 2U));

    if(samples == 0U)
    {
        return (uint16_t)(bar_x + (bar_width / 2U));
    }

    if(value > samples)
    {
        value = samples;
    }

    return (uint16_t)(bar_x + (((uint64_t)value * (bar_width - 1U)) / samples));
}

static void Draw_I2S_Sync_Marker(uint16_t x, uint16_t center_y, uint16_t height, uint16_t color)
{
    constexpr uint16_t marker_width = 3U;
    uint16_t x0 = (x > 0U) ? (uint16_t)(x - 1U) : 0U;
    uint16_t x1 = (uint16_t)(x0 + marker_width - 1U);
    uint16_t y0 = (uint16_t)(center_y - (height / 2U));
    uint16_t y1 = (uint16_t)(y0 + height - 1U);

    if(x1 >= ST7789_GetWidth())
    {
        x1 = (uint16_t)(ST7789_GetWidth() - 1U);
    }

    ST7789_Fill(x0, y0, x1, y1, color);
}

static void TLV320_I2S_CheckBitslip(void)
{

    if(s_i2s_bitslip_check == 0U)
    {
        return;
    }

    if(!i2s_fft_sample_arr_ready())
    {
        return;
    }

    const uint32_t sample_threshold = s_i2s_bitslip_false_checks_in_a_row > 0 ? 60000 : 20000;
    if(i2s_coincidence_status().samples < sample_threshold)
    {
        return;
    }

    Draw_I2S_Sync_Status();
    bool i2s_reset_needed = i2s_needs_reset();
    bool iq_inverted = false;
    if(!i2s_reset_needed)
    {
        auto iq_powers = iq_calibration_measure_ready_block();
        // Should never happen
        if(!iq_powers) {
            return;
        }
        float minus_20khz_power = iq_powers->first;
        float plus_20khz_power = iq_powers->second;
        iq_inverted = minus_20khz_power < plus_20khz_power;
    }

    if(i2s_reset_needed || iq_inverted)
    {
        s_i2s_bitslip_false_checks_in_a_row = 0U;
        printf("%s, resetting\n", i2s_reset_needed ? "bitslipped" : "IQ inverted");
        i2s_hw_enable(DISABLE);
        i2s_hw_deinit();
        Delay_Ms(40);
        i2s_hw_init();
        i2s_hw_enable(ENABLE);
        s_tlv320_i2s_report_initialized = 0U;
        s_i2s_bitslip_check = 1U;
        return;
    }

    ++s_i2s_bitslip_false_checks_in_a_row;
    if(s_i2s_bitslip_false_checks_in_a_row >= 20U)
    {
        s_i2s_bitslip_check = 0U;
    }
}

static void Draw_I2S_Sync_Status(void)
{
    char sync_text[24];
    char coincidence_text[32];
    i2s_coincidence_status_t coincidence_status = i2s_coincidence_status();
    uint64_t now_tick = SysTick->CNT;
    static uint64_t last_draw_tick = 0U;
    constexpr uint16_t bar_x = 18U;
    constexpr uint16_t bar_y = 120U;
    const uint16_t bar_width = (uint16_t)(ST7789_GetWidth() - (bar_x * 2U));
    constexpr uint16_t orange = 0xFD20;

    if((s_i2s_bitslip_sync_displayed_count == s_i2s_bitslip_false_checks_in_a_row) &&
       ((now_tick - last_draw_tick) < ticks_from_ms(100U)))
    {
        return;
    }

    last_draw_tick = now_tick;
    s_i2s_bitslip_sync_displayed_count = s_i2s_bitslip_false_checks_in_a_row;
    snprintf(sync_text, sizeof(sync_text),
             "Synced: %2u/20",
             (unsigned int)s_i2s_bitslip_false_checks_in_a_row);
    snprintf(coincidence_text, sizeof(coincidence_text),
             "Coinc: %5lu/%5lu",
             (unsigned long)coincidence_status.coincidences,
             (unsigned long)coincidence_status.samples);

    ST7789_WriteString(0U, 5U, "Initializing...", Font_11x18, WHITE, BLACK);
    ST7789_WriteString(0U, 27U, sync_text, Font_11x18, WHITE, BLACK);
    ST7789_WriteString(0U, 49U, coincidence_text, Font_11x18, WHITE, BLACK);
    ST7789_Fill((uint16_t)(bar_x - 1U),
                (uint16_t)(bar_y - 14U),
                (uint16_t)(bar_x + bar_width),
                (uint16_t)(bar_y + 13U),
                BLACK);
    ST7789_Fill(bar_x, bar_y, (uint16_t)(bar_x + bar_width - 1U), (uint16_t)(bar_y + 1U), GRAY);
    Draw_I2S_Sync_Marker(I2S_Sync_Bar_X(coincidence_status.acceptable_min, coincidence_status.samples), bar_y, 14U, orange);
    Draw_I2S_Sync_Marker(I2S_Sync_Bar_X(coincidence_status.acceptable_max, coincidence_status.samples), bar_y, 14U, orange);
    Draw_I2S_Sync_Marker(I2S_Sync_Bar_X(coincidence_status.coincidences, coincidence_status.samples), bar_y, 28U, WHITE);
}

static void ADC_Poll(void)
{
    uint32_t vdda = adc_hw_vdda_mv();
    uint16_t batt_raw = adc_hw_read_batt_raw();
    uint16_t vbus_raw = adc_hw_read_vbus_raw();
    uint16_t temp_raw = adc_hw_read_temp_raw();

    uint32_t batt_mv = (uint32_t)batt_raw * vdda * 2U / 4096U;
    uint32_t vbus_mv = (uint32_t)vbus_raw * vdda * 2U / 4096U;
    uint32_t temp_mv = (uint32_t)temp_raw * vdda / 4096U;
    int32_t temp_c = TempSensor_Volt_To_Temper((int32_t)temp_mv);

    printf("BATT: raw=%u %lu.%03lu V | VBUS: raw=%u %lu.%03lu V | TEMP: raw=%u %ld C\r\n",
           batt_raw, (unsigned long)(batt_mv / 1000U), (unsigned long)(batt_mv % 1000U),
           vbus_raw, (unsigned long)(vbus_mv / 1000U), (unsigned long)(vbus_mv % 1000U),
           temp_raw, (long)temp_c);
}

static void SDCard_PrintCIDAndSector0(void)
{
    if(sdcard::detect() != READY)
    {
        auto s = sdcard::status();
        printf("SD: status=%s bus=%u-bit clk=%lu Hz hs=%u\r\n",
               s.detected ? "detected" : "not detected",
               s.bus_width_bits,
               (unsigned long)s.clock_hz,
               s.high_speed ? 1U : 0U);
        printf("SD: not detected\r\n");
        return;
    }

    auto& c = sdcard::cid();
    printf("SD: %s %s MID=0x%02X PRV=%u.%u PSN=%lu %u/%02u\r\n",
           c.oid.data(), c.pnm.data(), c.mid,
           c.prv_major, c.prv_minor,
           (unsigned long)c.psn, c.mdt_year, c.mdt_month);
    auto s = sdcard::status();
    printf("SD: status=%s bus=%u-bit clk=%lu Hz hs=%u\r\n",
           s.detected ? "detected" : "not detected",
           s.bus_width_bits,
           (unsigned long)s.clock_hz,
           s.high_speed ? 1U : 0U);

    alignas(4) static uint8_t buf[512];
    if(sdcard::read_sector(0, buf) == READY)
    {
        printf("SD: sector 0:\r\n");
        for(uint32_t row = 0; row < 2; ++row)
        {
            printf("%03lX:", (unsigned long)(row * 32U));
            for(uint32_t col = 0; col < 32; ++col)
                printf(" %02X", buf[row * 32U + col]);
            printf("\r\n");
        }
    }
    else
    {
        printf("SD: sector 0 read failed\r\n");
    }
}

/*********************************************************************
 * @fn      main
 *
 * @brief   Main program.
 *
 * @return  none
 */
int main(void)
{
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
    SystemCoreClockUpdate();
    Delay_Init();

    usb_hw_init();
    
    //SysTick_Config(SystemCoreClock / 1000);	
    printf("SystemClk:%ld\r\n", SystemCoreClock);
    printf( "ChipID:%08lx\r\n", DBGMCU_GetCHIPID() );
    
    printf("ST7789 init\r\n");
    ST7789_Init();
    Boot_Display_InitLandscape();

    i2c_hw_init();
    si5351_init();
    
    if(si5351_hw_clk0_set_freq_hz(InitialCalibrationFreq) == READY)
    {
        printf("Si5351: LO CLK0/CLK1 = %lu Hz, CLK1 = +90 deg\r\n",
               (unsigned long)InitialCalibrationFreq);
    }
    else
    {
        printf("Si5351: LO program failed (I2C 0x60)\r\n");
    }

    if(tlv320adc6120_hw_init() == READY)
    {
        printf("TLV320ADC6120: I2C 0x4E, I2S controller 24-bit CH1+CH2, expects 24 MHz MCLK\r\n");
        GPIO_WriteBit(LED2_GPIO_PORT, LED2_GPIO_PIN, Bit_SET);
    }
    else
    {
        printf("TLV320ADC6120: I2C init failed (check wiring / AVDD AREG define / 24 MHz MCLK)\r\n");
    }

    if(cst328_hw_init() == READY)
    {
        printf("CST328: touch controller ready (I2C 0x1A)\r\n");
    }
    else
    {
        printf("CST328: init failed (check wiring / I2C 0x1A / TP_RST PC13 / IRQ PA12)\r\n");
    }

    i2s_hw_init();
    i2s_hw_enable(ENABLE);

    adc_hw_init();
    dac_hw_init();
    encoder_init();
    fm_audio_out_init();
    printf("FM audio out: enabled (fixed-point FM demod to DAC)\r\n");

    blinky_init();

    sdcard::init();
    
    //watchdog_init();

    PeriodicTrigger I2SPoll{1000U, TLV320_I2S_Poll};
    PeriodicTrigger I2CBusScan{1000U, Scan_I2CBus_EverySecond};
    PeriodicTrigger SysTickReportUSB{1000U, SysTick_Report_USB_EverySecond};
    PeriodicTrigger ADCPoll{1000U, ADC_Poll};
    PeriodicTrigger SDCardPoll{1000U, SDCard_PrintCIDAndSector0};

    // Set to min gain to allow calibration to take place
    si5351_hw_clk0_set_freq_hz(InitialCalibrationFreq);
    (void)tlv320adc6120_hw_set_ch_gain_db_x2(-100);
    while(s_i2s_bitslip_check)
    {
        TLV320_I2S_CheckBitslip();
        tud_task();
    }

    ST7789_Fill_Color(BLACK);
    i2s_coincidence_disable();

    while(iq_calibration_run())
    {
        iq_calibration_display();
        tud_task();
    }

    (void)tlv320adc6120_hw_set_ch_gain_db_x2(0);
    usb_hw_set_clk_freq_hz(InitialFMFreq);
    UI_Init();

    while(1)
    {
        I2SPoll();
        //I2CBusScan();
        //SysTickReportUSB();
        UI_Draw();
        tud_task();
        ADCPoll();
        cst328_hw_poll();
        blinky_task();
        SDCardPoll();
    }
}
