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

/*********************************************************************
 * @fn      GPIO_Toggle_INIT
 *
 * @brief   Initializes GPIOA.0
 *
 * @return  none
 */
void GPIO_Toggle_INIT(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOC, ENABLE);
    GPIO_InitStructure.GPIO_Pin = LED_GPIO_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(LED_GPIO_PORT, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = BOOT_GPIO_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPD;
    GPIO_Init(BOOT_GPIO_PORT, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = LED1_GPIO_PIN | LED2_GPIO_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(LED1_GPIO_PORT, &GPIO_InitStructure);
    GPIO_WriteBit(LED1_GPIO_PORT, LED1_GPIO_PIN, Bit_RESET);
    GPIO_WriteBit(LED2_GPIO_PORT, LED2_GPIO_PIN, Bit_RESET);
}

static uint64_t led_blink_period_ticks = 0;
static uint64_t led_last_toggle_tick = 0;
static BitAction led_state = Bit_RESET;

static uint64_t led12_blink_period_ticks = 0;
static uint64_t led12_last_toggle_tick = 0;
static BitAction led12_state = Bit_RESET;

static void SysTick_FreeRun_Init(void)
{
    /* Use the known-good WCH mode (same style as TinyUSB BSP SysTick_Config). */
    SysTick->CTLR = 0;
    SysTick->SR = 0;
    SysTick->CNT = 0;
    SysTick->CMP = 0xFFFFFFFFFFFFFFFFULL;
    SysTick->CTLR = 0x0F;
}

static void LED_Blink_Init(uint32_t period_ms)
{
    if(period_ms == 0U)
    {
        period_ms = 1U;
    }

    led_blink_period_ticks = ((uint64_t)SystemCoreClock * (uint64_t)period_ms) / 1000ULL;
    if(led_blink_period_ticks == 0U)
    {
        led_blink_period_ticks = 1U;
    }

    led_last_toggle_tick = SysTick->CNT;
    led_state = Bit_RESET;
    GPIO_WriteBit(LED_GPIO_PORT, LED_GPIO_PIN, led_state);

    led12_blink_period_ticks = ((uint64_t)SystemCoreClock * (uint64_t)period_ms) / 1000ULL;
    if(led12_blink_period_ticks == 0U)
    {
        led12_blink_period_ticks = 1U;
    }
    led12_last_toggle_tick = SysTick->CNT;
    led12_state = Bit_RESET;
    GPIO_WriteBit(LED1_GPIO_PORT, LED1_GPIO_PIN, led12_state);
    //GPIO_WriteBit(LED2_GPIO_PORT, LED2_GPIO_PIN, led12_state);
}

static void LED_Blink_Task(void)
{
    uint64_t now_tick = SysTick->CNT;

    if((now_tick - led_last_toggle_tick) >= led_blink_period_ticks)
    {
        led_last_toggle_tick = now_tick;
        led_state = (led_state == Bit_RESET) ? Bit_SET : Bit_RESET;
        GPIO_WriteBit(LED_GPIO_PORT, LED_GPIO_PIN, led_state);
    }

    if((now_tick - led12_last_toggle_tick) >= led12_blink_period_ticks)
    {
        led12_last_toggle_tick = now_tick;
        led12_state = (led12_state == Bit_RESET) ? Bit_SET : Bit_RESET;
        GPIO_WriteBit(LED1_GPIO_PORT, LED1_GPIO_PIN, led12_state);
        //GPIO_WriteBit(LED2_GPIO_PORT, LED2_GPIO_PIN, led12_state);
    }
}

static void SysTick_Report_USB_EverySecond(void)
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

static void Scan_I2CBus_EverySecond(void)
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

    //SysTick_Config(SystemCoreClock / 1000);	
    printf("SystemClk:%ld\r\n", SystemCoreClock);
    printf( "ChipID:%08lx\r\n", DBGMCU_GetCHIPID() );

    printf("GPIO Toggle TEST\r\n");
    GPIO_Toggle_INIT();


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
        printf("TLV320ADC6120: I2C 0x4E, I2S slave 16-bit CH1+CH2\r\n");
        GPIO_WriteBit(LED2_GPIO_PORT, LED2_GPIO_PIN, Bit_SET);
    }
    else
    {
        printf("TLV320ADC6120: I2C init failed (check wiring / AVDD AREG define)\r\n");
    }

    if(si5351_hw_fm_lo_both_hz(94000ULL) == READY)
    {
        printf("Si5351: FM LO CLK0+CLK1 = 94000 Hz (Taylor / demux)\r\n");
    }
    else
    {
        printf("Si5351: LO program failed (I2C 0x60)\r\n");
    }

    i2s_hw_init();
    i2s_hw_enable(ENABLE);

    dac_hw_init();
    dac_hw_static_noise_start(48000U);
    printf("DAC: static noise PA4+PA5 @ 48 ksps (TIM7 xorshift)\r\n");

    SysTick_FreeRun_Init();
    usb_hw_init();
    LED_Blink_Init(1000U);

    /* display_spi_test_run(); */

    while(1)
    {
        usb_hw_task();
        //Scan_I2CBus_EverySecond();
        //SysTick_Report_USB_EverySecond();
        LED_Blink_Task();
    }
}
