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
#include <functional>
#include <type_traits>
#include <utility>

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
#include "feature/blinky/blinky.h"
#include "feature/fm_audio_out/fm_audio_out.h"
#include "ui/fft.h"
#include "ui/ui.h"
}

#include "tusb.h"

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
static uint64_t ticks_from_ms(uint32_t ms);

template<typename Callable>
class PeriodicTrigger
{
public:
    template<typename F>
    constexpr PeriodicTrigger(uint32_t trigger_ms, F&& f) :
        trigger_ms(trigger_ms),
        f(std::forward<F>(f))
    {
    }

    void operator()()
    {
        uint64_t now_tick = SysTick->CNT;
        uint64_t trigger_period_ticks = ticks_from_ms(trigger_ms);

        if((now_tick - last_trigger_tick) < trigger_period_ticks)
        {
            return;
        }

        last_trigger_tick = now_tick;
        std::invoke(f);
        return;
    }

private:
    const uint32_t trigger_ms;
    uint64_t last_trigger_tick = 0U;
    Callable f;
};

template<typename F>
PeriodicTrigger(uint32_t, F&&) -> PeriodicTrigger<std::decay_t<F>>;

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

static void TLV320_I2S_CheckBitslip(void)
{

    if(s_i2s_bitslip_check == 0U)
    {
        return;
    }

    if(i2s_needs_reset())
    {
        s_i2s_bitslip_false_checks_in_a_row = 0U;
        printf("bitslipped, resetting\n");
        i2s_hw_enable(DISABLE);
        i2s_hw_deinit();
        Delay_Ms(10);
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

    if(s_i2s_bitslip_sync_displayed_count == s_i2s_bitslip_false_checks_in_a_row)
    {
        return;
    }

    s_i2s_bitslip_sync_displayed_count = s_i2s_bitslip_false_checks_in_a_row;
    snprintf(sync_text, sizeof(sync_text),
             "Synced: %2u/20",
             (unsigned int)s_i2s_bitslip_false_checks_in_a_row);

    ST7789_WriteString(0U, 5U, "Initializing...", Font_11x18, WHITE, BLACK);
    ST7789_WriteString(0U, 27U, sync_text, Font_11x18, WHITE, BLACK);
}

static uint64_t ticks_from_ms(uint32_t ms)
{
    uint64_t ticks = ((uint64_t)SystemCoreClock * (uint64_t)ms) / 1000ULL;
    if(ticks == 0U)
    {
        ticks = 1U;
    }
    return ticks;
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

    i2c_hw_init();

    if(cst328_hw_init() == READY)
    {
        printf("CST328: touch controller ready (I2C 0x1A)\r\n");
    }
    else
    {
        printf("CST328: init failed (check wiring / I2C 0x1A / TP_RST PC13 / IRQ PA12)\r\n");
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

    constexpr uint64_t kInitialLoFreqHz = 93300000ULL;
    if(usb_hw_set_clk_freq_hz(kInitialLoFreqHz) == READY)
    {
        printf("Si5351: LO CLK0/CLK1 = %lu Hz, CLK1 = +90 deg\r\n",
               (unsigned long)kInitialLoFreqHz);
    }
    else
    {
        printf("Si5351: LO program failed (I2C 0x60)\r\n");
    }

    i2s_hw_init();
    i2s_hw_enable(ENABLE);

    dac_hw_init();
    encoder_init();
    fm_audio_out_init();
    printf("FM audio out: enabled (fixed-point FM demod to DAC)\r\n");

    blinky_init();

    //watchdog_init();

    PeriodicTrigger I2SBitslipCheck{100U, TLV320_I2S_CheckBitslip};
    PeriodicTrigger I2SPoll{1000U, TLV320_I2S_Poll};
    PeriodicTrigger I2CBusScan{1000U, Scan_I2CBus_EverySecond};
    PeriodicTrigger SysTickReportUSB{1000U, SysTick_Report_USB_EverySecond};
    PeriodicTrigger FFTDraw{1000U / 60U, UI_FFT_Draw};

    while(s_i2s_bitslip_check)
    {
        Draw_I2S_Sync_Status();
        I2SBitslipCheck();
        tud_task();
    }
    
    UI_FFT_Init();
    UI_Init();

    while(1)
    {
        I2SBitslipCheck();
        I2SPoll();
        //I2CBusScan();
        //SysTickReportUSB();
        UI_Draw();
        tud_task();
        FFTDraw();
        cst328_hw_poll();
        blinky_task();
        //watchdog_kick();
    }
}
