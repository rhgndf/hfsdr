#include "dac_hw.h"

#include "hw/pinout.h"

#include "ch32v30x_dac.h"
#include "ch32v30x_gpio.h"
#include "ch32v30x_misc.h"
#include "ch32v30x_rcc.h"
#include "ch32v30x_tim.h"

static uint16_t sq_low;
static uint16_t sq_high;
static uint8_t sq_phase;

static uint32_t tim6_counter_clock_hz(void)
{
    RCC_ClocksTypeDef clk = {0};
    RCC_GetClocksFreq(&clk);
    /*
     * APB1 timers: same rule as STM32 — if APB1 prescaler is not /1, timer counter
     * clock is 2×PCLK1; otherwise it equals PCLK1.
     */
    uint32_t pclk1 = clk.PCLK1_Frequency;
    uint32_t ppre1 = (RCC->CFGR0 & RCC_PPRE1) >> 8;
    if(ppre1 != 0U)
    {
        return pclk1 * 2U;
    }
    return pclk1;
}

void dac_hw_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    DAC_InitTypeDef dac = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_DAC, ENABLE);

    gpio.GPIO_Pin = DAC1_OUT_GPIO_PIN | DAC2_OUT_GPIO_PIN;
    gpio.GPIO_Mode = GPIO_Mode_AIN;
    GPIO_Init(DAC1_OUT_GPIO_PORT, &gpio);

    DAC_StructInit(&dac);
    dac.DAC_Trigger = DAC_Trigger_None;
    dac.DAC_WaveGeneration = DAC_WaveGeneration_None;
    dac.DAC_OutputBuffer = DAC_OutputBuffer_Enable;

    DAC_Init(DAC_Channel_1, &dac);
    DAC_Init(DAC_Channel_2, &dac);
    DAC_Cmd(DAC_Channel_1, ENABLE);
    DAC_Cmd(DAC_Channel_2, ENABLE);

    DAC_SetChannel1Data(DAC_Align_12b_R, 2048U);
    DAC_SetChannel2Data(DAC_Align_12b_R, 2048U);
}

void dac_hw_set_channel1_12(uint16_t value)
{
    if(value > 4095U)
    {
        value = 4095U;
    }
    DAC_SetChannel1Data(DAC_Align_12b_R, value);
}

void dac_hw_set_channel2_12(uint16_t value)
{
    if(value > 4095U)
    {
        value = 4095U;
    }
    DAC_SetChannel2Data(DAC_Align_12b_R, value);
}

void dac_hw_set_both_12(uint16_t value)
{
    if(value > 4095U)
    {
        value = 4095U;
    }
    DAC_SetDualChannelData(DAC_Align_12b_R, value, value);
}

void dac_hw_square_wave_stop(void)
{
    TIM_Cmd(TIM6, DISABLE);
    TIM_ITConfig(TIM6, TIM_IT_Update, DISABLE);
    NVIC_DisableIRQ(TIM6_IRQn);
    DAC_SetDualChannelData(DAC_Align_12b_R, sq_low, sq_low);
    sq_phase = 0;
}

void dac_hw_square_wave_start(uint32_t freq_hz, uint16_t low_12, uint16_t high_12)
{
    TIM_TimeBaseInitTypeDef t = {0};
    NVIC_InitTypeDef nvic = {0};
    uint32_t timclk;
    uint32_t ticks_half;
    uint32_t psc;
    uint32_t arr;

    dac_hw_square_wave_stop();

    if(low_12 > 4095U)
    {
        low_12 = 4095U;
    }
    if(high_12 > 4095U)
    {
        high_12 = 4095U;
    }
    sq_low = low_12;
    sq_high = high_12;
    sq_phase = 0;
    DAC_SetDualChannelData(DAC_Align_12b_R, sq_low, sq_low);

    if(freq_hz == 0U)
    {
        return;
    }

    timclk = tim6_counter_clock_hz();
    ticks_half = timclk / (2U * freq_hz);
    if(ticks_half == 0U)
    {
        ticks_half = 1U;
    }

    psc = 0U;
    arr = ticks_half - 1U;
    while(arr > 0xFFFFU && psc < 0xFFFFU)
    {
        ++psc;
        arr = (ticks_half / (psc + 1U)) - 1U;
    }
    if(arr > 0xFFFFU)
    {
        arr = 0xFFFFU;
    }

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM6, ENABLE);
    TIM_DeInit(TIM6);
    TIM_TimeBaseStructInit(&t);
    t.TIM_Period = (uint16_t)arr;
    t.TIM_Prescaler = (uint16_t)psc;
    t.TIM_ClockDivision = TIM_CKD_DIV1;
    t.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM6, &t);
    TIM_ClearFlag(TIM6, TIM_FLAG_Update);
    TIM_ITConfig(TIM6, TIM_IT_Update, ENABLE);

    nvic.NVIC_IRQChannel = TIM6_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 2;
    nvic.NVIC_IRQChannelSubPriority = 0;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);

    TIM_Cmd(TIM6, ENABLE);
}

__attribute__((interrupt)) void TIM6_IRQHandler(void)
{
    if(TIM_GetITStatus(TIM6, TIM_IT_Update) != RESET)
    {
        TIM_ClearITPendingBit(TIM6, TIM_IT_Update);
        sq_phase ^= 1U;
        if(sq_phase != 0U)
        {
            DAC_SetDualChannelData(DAC_Align_12b_R, sq_high, sq_high);
        }
        else
        {
            DAC_SetDualChannelData(DAC_Align_12b_R, sq_low, sq_low);
        }
    }
}
