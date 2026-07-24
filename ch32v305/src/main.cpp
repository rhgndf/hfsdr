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
#include "main.h"
#include <stddef.h>

extern "C" {
#include "FreeRTOS.h"
#include "task.h"

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
#include "hw/rtc.h"
#include "hw/trng.h"
#include "feature/blinky/blinky.h"
#include "demod/demod.h"
#include "ui/ui.h"
}

#include "feature/iq_calibration/iq_calibration.h"
#include "demod/rds.h"
#include "hw/sdcard/sdcard.h"
#include "utils/utils.h"
#include "freertos/task_stacks.h"


constexpr uint64_t InitialCalibrationFreq = 144020000ULL;
constexpr uint64_t InitialFMFreq = 92400000ULL;
static void Boot_Display_InitLandscape(void);

static hardware_rev_t s_hardware_rev = HARDWARE_REV_UNKNOWN;
static StaticTask_t s_i2s_task_tcb;
static StackType_t s_i2s_task_stack[I2S_TASK_STACK_WORDS]
    __attribute__((aligned(portBYTE_ALIGNMENT)));
static StaticTask_t s_usb_task_tcb;
static StackType_t s_usb_task_stack[USB_TASK_STACK_WORDS]
    __attribute__((aligned(portBYTE_ALIGNMENT)));
static StaticTask_t s_application_task_tcb;
static StackType_t s_application_task_stack[APP_TASK_STACK_WORDS]
    __attribute__((aligned(portBYTE_ALIGNMENT)));

void detect_hardware_rev(void)
{
    GPIO_InitTypeDef gpio{};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);

    gpio.GPIO_Pin = GPIO_Pin_9;
    gpio.GPIO_Mode = GPIO_Mode_IPD;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &gpio);

    Delay_Us(10U);

    s_hardware_rev = (GPIO_ReadInputDataBit(GPIOB, GPIO_Pin_9) != 0U) ? HARDWARE_REV_V2 : HARDWARE_REV_V1;
}

hardware_rev_t get_hardware_rev(void)
{
    return s_hardware_rev;
}

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
static uint8_t s_i2s_bitslip_sync_dots = 0U;
static uint32_t s_i2s_bitslip_checked_word_count = 0U;

static void Boot_Display_InitLandscape(void)
{
    ST7789_SetRotation(3U);
    ST7789_VerticalScrollDisable();
    ST7789_Fill_Color(BLACK);
}

static void Draw_I2S_Syncing_Status(void)
{
    constexpr const char *sync_text[] = {
        "Syncing   ",
        "Syncing.  ",
        "Syncing.. ",
        "Syncing..."
    };

    ST7789_WriteString(0U, 5U, sync_text[s_i2s_bitslip_sync_dots], Font_11x18, WHITE, BLACK);
}

static void TLV320_I2S_CheckBitslip(void)
{
    if(s_i2s_bitslip_check == 0U)
    {
        return;
    }


    static uint32_t i2s_last_rx_word = 0;
    uint32_t i2s_cur_rx_word = i2s_hw_rx_word_count();
    if (i2s_last_rx_word == i2s_cur_rx_word) {
        return;
    }
    i2s_last_rx_word = i2s_cur_rx_word;

    bool i2s_reset_needed = i2s_needs_reset();
    if(i2s_reset_needed)
    {
        s_i2s_bitslip_checked_word_count = 0U;
        s_i2s_bitslip_sync_dots = (uint8_t)((s_i2s_bitslip_sync_dots + 1U) & 3U);
        Draw_I2S_Syncing_Status();
        printf("bitslipped, resetting\n");
        i2s_hw_enable(DISABLE);
        i2s_hw_deinit();
        // Random wait
        uint32_t wait_amt = trng_hw_read_u32()%1000000;
        while(wait_amt--) {
            asm volatile("nop");
        }
        i2s_hw_init();
        i2s_hw_enable(ENABLE);
        s_tlv320_i2s_report_initialized = 0U;
        s_i2s_bitslip_check = 1U;
        return;
    }

    uint32_t word_count = i2s_hw_rx_word_count();
    if(word_count == s_i2s_bitslip_checked_word_count)
    {
        return;
    }

    s_i2s_bitslip_checked_word_count = word_count;
    s_i2s_bitslip_check = 0U;
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

static void SDCard_Poll(void)
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
}

static void Application_Task(void *parameters)
{
    (void)parameters;

    //rtc_init();
    detect_hardware_rev();
    
    printf("SystemClk:%ld\r\n", SystemCoreClock);
    printf( "ChipID:%08lx\r\n", DBGMCU_GetCHIPID() );
    printf("Hardware rev:v%u\r\n", (unsigned int)get_hardware_rev());
    
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
    demod_init();
    printf("Demod audio out: enabled (WBFM/NBFM/AM/USB/LSB to DAC)\r\n");

    blinky_init();

    sdcard::init();

    PeriodicTrigger I2SPoll{1000U, TLV320_I2S_Poll};
    PeriodicTrigger I2CBusScan{1000U, Scan_I2CBus_EverySecond};
    PeriodicTrigger SysTickReportUSB{1000U, SysTick_Report_USB_EverySecond};
    PeriodicTrigger ADCPoll{1000U, ADC_Poll};
    PeriodicTrigger SDCardPoll{1000U, SDCard_Poll};
    PeriodicTrigger DACBufferAdjust{1000U, dac_hw_stream_adjust_buffer};
    si5351_hw_clk0_set_freq_hz(InitialCalibrationFreq);

    trng_hw_init();
    (void)tlv320adc6120_ch1_mute(true);
    Draw_I2S_Syncing_Status();
    while(s_i2s_bitslip_check)
    {
        TLV320_I2S_CheckBitslip();
        vTaskDelay(pdMS_TO_TICKS(1U));
    }
    (void)tlv320adc6120_ch1_mute(false);
    trng_hw_deinit();
    
    ST7789_Fill_Color(BLACK);
    i2s_sync_check_disable();

    (void)tlv320adc6120_hw_set_ch_gain_db_x2(-100);
    while(iq_calibration_run())
    {
        iq_calibration_display();
    }

    (void)tlv320adc6120_hw_set_ch_gain_db_x2(0);
    usb_hw_set_clk_freq_hz(InitialFMFreq);
    UI_Init();
    watchdog_kick();

    while(1)
    {
        I2SPoll();
        DACBufferAdjust();
        //I2CBusScan();
        //SysTickReportUSB();
        UI_Draw();
        //ADCPoll();
        //cst328_hw_poll();
        blinky_task();
        SDCardPoll();
        //demod::rds_poll();
        watchdog_kick();
        vTaskDelay(pdMS_TO_TICKS(2U));
    }
}

/*********************************************************************
 * @fn      main
 *
 * @brief   Configure the scheduler and create all statically allocated tasks.
 *
 * @return  none
 */
int main(void)
{
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
    SystemCoreClockUpdate();
    configASSERT(SystemCoreClock == configCPU_CLOCK_HZ);
    Delay_Init();

    TaskHandle_t i2s_task_handle =
        xTaskCreateStatic(i2s_task,
                          "i2s",
                          I2S_TASK_STACK_WORDS,
                          nullptr,
                          3U,
                          s_i2s_task_stack,
                          &s_i2s_task_tcb);
    configASSERT(i2s_task_handle != nullptr);

    TaskHandle_t usb_task_handle =
        xTaskCreateStatic(usb_task,
                          "usb",
                          USB_TASK_STACK_WORDS,
                          nullptr,
                          2U,
                          s_usb_task_stack,
                          &s_usb_task_tcb);
    configASSERT(usb_task_handle != nullptr);

    TaskHandle_t application_task =
        xTaskCreateStatic(Application_Task,
                          "application",
                          APP_TASK_STACK_WORDS,
                          nullptr,
                          1U,
                          s_application_task_stack,
                          &s_application_task_tcb);
    configASSERT(application_task != nullptr);

    vTaskStartScheduler();
    configASSERT(false);
    for(;;)
    {
    }
}
