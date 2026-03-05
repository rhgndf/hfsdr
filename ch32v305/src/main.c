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

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    GPIO_InitStructure.GPIO_Pin = LED_GPIO_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(LED_GPIO_PORT, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = BOOT_GPIO_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPD;
    GPIO_Init(BOOT_GPIO_PORT, &GPIO_InitStructure);
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
    printf("SystemClk:%d\r\n", SystemCoreClock);
    printf( "ChipID:%08x\r\n", DBGMCU_GetCHIPID() );

    printf("GPIO Toggle TEST\r\n");
    GPIO_Toggle_INIT();

    spi_hw_init();
    i2c_hw_init();
    i2s_hw_init();
    i2s_hw_enable(ENABLE);

    usb_hw_init();

    while(1)
    {
        (void)i2s_hw_try_receive_u16(&i2s_sample);
        usb_hw_task();
    }
}
