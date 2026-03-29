#include "dac_hw_sine_test.h"

#include "hw/dac_hw.h"

#include "ch32v30x_dac.h"
#include "ch32v30x_misc.h"
#include "ch32v30x_rcc.h"
#include "ch32v30x_tim.h"

/* 256 samples, centered at 2048, amplitude ~1900 (fits 0..4095) */
static const uint16_t sine_lut[256] = {
    2048, 2095, 2141, 2188, 2234, 2281, 2327, 2373, 2419, 2464, 2510, 2555, 2600, 2644, 2688, 2732,
    2775, 2818, 2860, 2902, 2944, 2985, 3025, 3064, 3104, 3142, 3180, 3217, 3253, 3289, 3324, 3358,
    3392, 3424, 3456, 3487, 3517, 3546, 3574, 3601, 3628, 3653, 3678, 3701, 3724, 3745, 3766, 3785,
    3803, 3821, 3837, 3852, 3866, 3879, 3891, 3902, 3911, 3920, 3927, 3934, 3939, 3943, 3946, 3947,
    3948, 3947, 3946, 3943, 3939, 3934, 3927, 3920, 3911, 3902, 3891, 3879, 3866, 3852, 3837, 3821,
    3803, 3785, 3766, 3745, 3724, 3701, 3678, 3653, 3628, 3601, 3574, 3546, 3517, 3487, 3456, 3424,
    3392, 3358, 3324, 3289, 3253, 3217, 3180, 3142, 3104, 3064, 3025, 2985, 2944, 2902, 2860, 2818,
    2775, 2732, 2688, 2644, 2600, 2555, 2510, 2464, 2419, 2373, 2327, 2281, 2234, 2188, 2141, 2095,
    2048, 2001, 1955, 1908, 1862, 1815, 1769, 1723, 1677, 1632, 1586, 1541, 1496, 1452, 1408, 1364,
    1321, 1278, 1236, 1194, 1152, 1111, 1071, 1032, 992, 954, 916, 879, 843, 807, 772, 738,
    704, 672, 640, 609, 579, 550, 522, 495, 468, 443, 418, 395, 372, 351, 330, 311,
    293, 275, 259, 244, 230, 217, 205, 194, 185, 176, 169, 162, 157, 153, 150, 149,
    148, 149, 150, 153, 157, 162, 169, 176, 185, 194, 205, 217, 230, 244, 259, 275,
    293, 311, 330, 351, 372, 395, 418, 443, 468, 495, 522, 550, 579, 609, 640, 672,
    704, 738, 772, 807, 843, 879, 916, 954, 992, 1032, 1071, 1111, 1152, 1194, 1236, 1278,
    1321, 1364, 1408, 1452, 1496, 1541, 1586, 1632, 1677, 1723, 1769, 1815, 1862, 1908, 1955, 2001,
};

static uint32_t phase_acc;
static uint32_t phase_inc;

static uint32_t tim7_counter_clock_hz(void)
{
    RCC_ClocksTypeDef clk = {0};
    RCC_GetClocksFreq(&clk);
    uint32_t pclk1 = clk.PCLK1_Frequency;
    uint32_t ppre1 = (RCC->CFGR0 & RCC_PPRE1) >> 8;
    if(ppre1 != 0U)
    {
        return pclk1 * 2U;
    }
    return pclk1;
}

void dac_hw_sine_test_stop(void)
{
    TIM_Cmd(TIM7, DISABLE);
    TIM_ITConfig(TIM7, TIM_IT_Update, DISABLE);
    NVIC_DisableIRQ(TIM7_IRQn);
    DAC_SetDualChannelData(DAC_Align_12b_R, 2048U, 2048U);
}

void dac_hw_sine_test_start(uint32_t sine_freq_hz, uint32_t sample_rate_hz)
{
    TIM_TimeBaseInitTypeDef t = {0};
    NVIC_InitTypeDef nvic = {0};
    uint32_t timclk;
    uint32_t ticks;
    uint32_t psc;
    uint32_t arr;

    if(sample_rate_hz == 0U || sine_freq_hz == 0U)
    {
        dac_hw_sine_test_stop();
        return;
    }

    dac_hw_square_wave_stop();
    dac_hw_sine_test_stop();

    phase_acc = 0U;
    phase_inc = (uint32_t)(((uint64_t)sine_freq_hz << 32) / (uint64_t)sample_rate_hz);

    timclk = tim7_counter_clock_hz();
    ticks = timclk / sample_rate_hz;
    if(ticks == 0U)
    {
        ticks = 1U;
    }

    psc = 0U;
    arr = ticks - 1U;
    while(arr > 0xFFFFU && psc < 0xFFFFU)
    {
        ++psc;
        arr = (ticks / (psc + 1U)) - 1U;
    }
    if(arr > 0xFFFFU)
    {
        arr = 0xFFFFU;
    }

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM7, ENABLE);
    TIM_DeInit(TIM7);
    TIM_TimeBaseStructInit(&t);
    t.TIM_Period = (uint16_t)arr;
    t.TIM_Prescaler = (uint16_t)psc;
    t.TIM_ClockDivision = TIM_CKD_DIV1;
    t.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM7, &t);
    TIM_ClearFlag(TIM7, TIM_FLAG_Update);
    TIM_ITConfig(TIM7, TIM_IT_Update, ENABLE);

    nvic.NVIC_IRQChannel = TIM7_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 2;
    nvic.NVIC_IRQChannelSubPriority = 1;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);

    TIM_Cmd(TIM7, ENABLE);
}

__attribute__((interrupt)) void TIM7_IRQHandler(void)
{
    uint32_t idx;
    uint16_t s;

    if(TIM_GetITStatus(TIM7, TIM_IT_Update) != RESET)
    {
        TIM_ClearITPendingBit(TIM7, TIM_IT_Update);
        phase_acc += phase_inc;
        idx = phase_acc >> 24;
        s = sine_lut[idx];
        DAC_SetDualChannelData(DAC_Align_12b_R, s, s);
    }
}
