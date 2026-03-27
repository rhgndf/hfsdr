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
#include "hw/pinout.h"
#include "hw/spi_hw.h"
#include "hw/i2c_hw.h"
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
}

static uint64_t led_blink_period_ticks = 0;
static uint64_t led_last_toggle_tick = 0;
static BitAction led_state = Bit_RESET;

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



    char msg[64];
    int len = snprintf(msg, sizeof(msg),
                       "%d systick_now=0x%lx last=0x%lx %d\r\n",
                       25, 
                       (uint32_t)now_tick,
                       (uint32_t)last_report_tick,
                       25);

                       
    if(len > 0)
    {
        (void)usb_send_data((uint8_t const *)msg, (uint32_t)len);
    }

    last_report_tick = now_tick;
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
    uint16_t i2s_sample = 0;

    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
    SystemCoreClockUpdate();
    Delay_Init();

    //SysTick_Config(SystemCoreClock / 1000);
    USART_Printf_Init(115200);	
    printf("SystemClk:%ld\r\n", SystemCoreClock);
    printf( "ChipID:%08lx\r\n", DBGMCU_GetCHIPID() );

    printf("GPIO Toggle TEST\r\n");
    GPIO_Toggle_INIT();

    /*spi_hw_init();
    i2c_hw_init();
    i2s_hw_init();
    i2s_hw_enable(ENABLE);*/

    SysTick_FreeRun_Init();
    usb_hw_init();
    //LED_Blink_Init(1000U);

    while(1)
    {
        //(void)i2s_hw_try_receive_u16(&i2s_sample);
        usb_hw_task();
        //SysTick_Report_USB_EverySecond();
        //LED_Blink_Task();
    }
}
