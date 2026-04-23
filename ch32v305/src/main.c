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
#include "hw/pinout.h"
#include "hw/dac_hw.h"
#include "hw/spi_manual.h"
#include "hw/st7789/st7789.h"
/* #include "test/spi_gpio_pins.h" */
/* #include "test/display_spi_test.h" */
#include "test/dac_hw_sine_test.h"
#include "hw/i2c_hw.h"
#include "hw/tlv320adc6120_hw.h"
#include "hw/si5351_hw.h"
#include "hw/i2s_hw.h"
#include "hw/usb_hw.h"
#include "hw/watchdog.h"
#include "feature/blinky/blinky.h"
#include "feature/fm_audio_out/fm_audio_out.h"
#include "tusb.h"

/*********************************************************************
 * @fn      GPIO_Toggle_INIT
 *
 * @brief   Initializes GPIOA.0
 *
 * @return  none
 */
void GPIO_Toggle_INIT(void)
{
    blinky_gpio_init();
}

[[maybe_unused]] static void TP_Reset_Pin_On(void)
{
    GPIO_InitTypeDef gpio_init = {0};

    /* TP/LCD reset net is active-low: drive high to release ("on"). */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC, ENABLE);
    gpio_init.GPIO_Pin = TP_RST_GPIO_PIN;
    gpio_init.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio_init.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(TP_RST_GPIO_PORT, &gpio_init);
    GPIO_WriteBit(TP_RST_GPIO_PORT, TP_RST_GPIO_PIN, Bit_SET);
}

[[maybe_unused]] static void TP_Reset_Pin_Off(void)
{
    GPIO_InitTypeDef gpio_init = {0};

    /* TP/LCD reset net is active-low: drive high to release ("on"). */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC, ENABLE);
    gpio_init.GPIO_Pin = TP_RST_GPIO_PIN;
    gpio_init.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio_init.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(TP_RST_GPIO_PORT, &gpio_init);
    GPIO_WriteBit(TP_RST_GPIO_PORT, TP_RST_GPIO_PIN, Bit_RESET);
}


[[maybe_unused]] static void SysTick_Report_USB_EverySecond(void)
{
    static uint64_t last_report_tick = 0;
    static uint8_t initialized = 0;
    uint64_t now_tick = SysTick->CNT;
    uint64_t report_period_ticks = (uint64_t)SystemCoreClock / 2;

    if(report_period_ticks == 0U)
    {
        report_period_ticks = 1U;
    }

    if(initialized == 0U)
    {
        last_report_tick = now_tick;
        initialized = 1U;
        return;
    }

    if((now_tick - last_report_tick) < report_period_ticks)
    {
        return;
    }

    printf("%d systick_now=0x%lx last=0x%lx %d\r\n",
           25,
           (uint32_t)now_tick,
           (uint32_t)last_report_tick,
           25);

    last_report_tick = now_tick;
}

[[maybe_unused]] static void Scan_I2CBus_EverySecond(void)
{
    static uint64_t last_scan_tick = 0;
    static uint8_t initialized = 0;
    uint64_t now_tick = SysTick->CNT;
    uint64_t scan_period_ticks = (uint64_t)SystemCoreClock;
    uint8_t addr = 0;
    uint8_t device_count = 0;

    if(scan_period_ticks == 0U)
    {
        scan_period_ticks = 1U;
    }

    if(initialized == 0U)
    {
        last_scan_tick = now_tick;
        initialized = 1U;
        return;
    }

    if((now_tick - last_scan_tick) < scan_period_ticks)
    {
        return;
    }

    printf("I2C scan:");

    for(addr = 0x08U; addr <= 0x77U; ++addr)
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

    last_scan_tick = now_tick;
}

/*********************************************************************
 * TLV320ADC6120 I2S capture (CH1/CH2)
 *
 * In the current 24-bit I2S mode, the CH32 peripheral uses 32-bit channel
 * frames, so each stereo frame arrives as four 16-bit DMA words.
 * SPI2 RX DMA runs in circular mode; DMA HT/TC interrupts count those words.
 * Report the incoming data rate once per second.
 *********************************************************************/
static void TLV320_I2S_Poll(void)
{
    static uint64_t last_report_tick = 0U;
    static uint32_t last_word_count = 0U;
    static uint32_t last_vendor_total_word_count = 0U;
    static uint32_t last_vendor_dropped_word_count = 0U;
    static uint8_t initialized = 0U;
    uint64_t now_tick;
    uint64_t elapsed_ticks;
    uint32_t words_now;
    uint32_t words_per_sec;
    uint32_t frames_per_sec;
    uint32_t bytes_per_sec;
    uint32_t vendor_words_now;
    uint32_t vendor_dropped_words_now;
    uint32_t vendor_words_per_sec;
    uint32_t vendor_dropped_words_per_sec;

    now_tick = SysTick->CNT;
    words_now = i2s_hw_rx_word_count();
    vendor_words_now = usb_hw_vendor_total_words();
    vendor_dropped_words_now = usb_hw_vendor_dropped_words();

    if(initialized == 0U)
    {
        last_report_tick = now_tick;
        last_word_count = words_now;
        last_vendor_total_word_count = vendor_words_now;
        last_vendor_dropped_word_count = vendor_dropped_words_now;
        initialized = 1U;
    }
    else
    {
        elapsed_ticks = now_tick - last_report_tick;
        if(elapsed_ticks >= (uint64_t)SystemCoreClock)
        {
            words_per_sec = (uint32_t)((((uint64_t)(words_now - last_word_count)) * (uint64_t)SystemCoreClock) / elapsed_ticks);
            frames_per_sec = words_per_sec / 4U;
            bytes_per_sec = words_per_sec * (uint32_t)sizeof(uint16_t);
            vendor_words_per_sec = (uint32_t)((((uint64_t)(vendor_words_now - last_vendor_total_word_count)) * (uint64_t)SystemCoreClock) / elapsed_ticks);
            vendor_dropped_words_per_sec = (uint32_t)((((uint64_t)(vendor_dropped_words_now - last_vendor_dropped_word_count)) * (uint64_t)SystemCoreClock) / elapsed_ticks);

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
            last_report_tick = now_tick;

            if(i2s_needs_reset())
            {
                printf("bitslipped, resetting\n");
                /*i2s_hw_deinit();
                i2s_hw_init();
                i2s_hw_enable(ENABLE);
                initialized = 0U;*/
            }
        }
    }
}

static void DAC_Poll(void)
{
    static uint64_t last_report_tick = 0U;
    static uint32_t last_frame_count = 0U;
    static uint8_t initialized = 0U;
    uint64_t now_tick;
    uint64_t elapsed_ticks;
    uint32_t frames_now;
    uint32_t frames_per_sec;
    uint32_t words_per_sec;
    uint32_t bytes_per_sec;

    now_tick = SysTick->CNT;
    frames_now = dac_hw_tx_frame_count();

    if(initialized == 0U)
    {
        last_report_tick = now_tick;
        last_frame_count = frames_now;
        initialized = 1U;
        return;
    }

    elapsed_ticks = now_tick - last_report_tick;
    if(elapsed_ticks < (uint64_t)SystemCoreClock)
    {
        return;
    }

    frames_per_sec = (uint32_t)((((uint64_t)(frames_now - last_frame_count)) * (uint64_t)SystemCoreClock) / elapsed_ticks);
    words_per_sec = frames_per_sec * 2U;
    bytes_per_sec = frames_per_sec * (uint32_t)sizeof(uint32_t);

    printf("DAC rate: %lu words/s, %lu frames/s, %lu B/s\r\n",
           (unsigned long)words_per_sec,
           (unsigned long)frames_per_sec,
           (unsigned long)bytes_per_sec);

    last_frame_count = frames_now;
    last_report_tick = now_tick;
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

/*
 * Toggle FM audio mode on each debounced encoder button press.
 * Encoder button is configured as GPIO_Mode_IPD in blinky_gpio_init(), so
 * pressed == logic 1.
 */
static void FmAudioOut_PollEncoderPress(void)
{
    static uint8_t initialized = 0U;
    static uint8_t raw_state = 0U;
    static uint8_t stable_state = 0U;
    static uint64_t last_change_tick = 0U;
    uint8_t new_raw;
    uint64_t now_tick;
    uint64_t debounce_ticks;

    now_tick = SysTick->CNT;
    debounce_ticks = ticks_from_ms(30U);
    new_raw = (uint8_t)GPIO_ReadInputDataBit(ENC_BTN_GPIO_PORT, ENC_BTN_GPIO_PIN);

    if(initialized == 0U)
    {
        raw_state = new_raw;
        stable_state = new_raw;
        last_change_tick = now_tick;
        initialized = 1U;
        return;
    }

    if(new_raw != raw_state)
    {
        raw_state = new_raw;
        last_change_tick = now_tick;
    }

    if((now_tick - last_change_tick) < debounce_ticks)
    {
        return;
    }

    if(stable_state == raw_state)
    {
        return;
    }

    stable_state = raw_state;
    if(stable_state != 0U)
    {
        bool new_state = !enable_fm_audio_out;
        fm_audio_out_set_enabled(new_state);
        printf("FM audio out: %s by encoder press\r\n", new_state ? "enabled" : "disabled");
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

    printf("GPIO Toggle TEST\r\n");
    GPIO_Toggle_INIT();
    // TP_Reset_Pin_Off();


    /*printf("SPI lines: GPIO mode, all outputs high (SCK MOSI CS RS RST)\r\n");
    spi_gpio_pins_enable();
    spi_gpio_pins_all_on();*/
    
    // printf("SPI: sample 0xFF (one CS-framed byte, command/RS low)\r\n");
    // spi_manual_init();
    // spi_manual_cs_begin();
    // spi_manual_rs_cmd();
    // (void)spi_manual_transfer_u8(0xFFU);
    // spi_manual_cs_end();
    printf("ST7789 init + built-in screen test\r\n");
    ST7789_Init();
    //ST7789_Test();

    i2c_hw_init();

    if(tlv320adc6120_hw_init() == READY)
    {
        printf("TLV320ADC6120: I2C 0x4E, I2S controller 24-bit CH1+CH2, expects 24 MHz MCLK\r\n");
        GPIO_WriteBit(LED2_GPIO_PORT, LED2_GPIO_PIN, Bit_SET);
    }
    else
    {
        printf("TLV320ADC6120: I2C init failed (check wiring / AVDD AREG define / 24 MHz MCLK)\r\n");
    }

    // if(usb_hw_set_clk_freq_hz(7067333ULL) == READY)
    if(usb_hw_set_clk_freq_hz(94400000ULL) == READY)
    {
        printf("Si5351: LO CLK0/CLK1 = 12000000 Hz, CLK1 = +90 deg\r\n");
    }
    else
    {
        printf("Si5351: LO program failed (I2C 0x60)\r\n");
    }

    i2s_hw_init();
    i2s_hw_enable(ENABLE);

    dac_hw_init();
    fm_audio_out_init();
    fm_audio_out_set_enabled(enable_fm_audio_out);
    if(enable_fm_audio_out)
    {
        printf("FM audio out: enabled (fixed-point FM demod to DAC)\r\n");
    }
    else
    {
        printf("DAC: A4 sine 440 Hz on PA4+PA5 @ 192 ksps\r\n");
    }

    blinky_init();

    /* display_spi_test_run(); */
    //watchdog_init();

    while(1)
    {
        TLV320_I2S_Poll();
        DAC_Poll();
        FmAudioOut_PollEncoderPress();
        tud_task();
        //Scan_I2CBus_EverySecond();
        //SysTick_Report_USB_EverySecond();
        blinky_task();
        //watchdog_kick();
    }
}
