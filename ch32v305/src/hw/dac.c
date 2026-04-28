#include "dac.h"

#include <assert.h>
#include <stddef.h>

#include "hw/pinout.h"

#include "ch32v30x_dac.h"
#include "ch32v30x_dma.h"
#include "ch32v30x_gpio.h"
#include "ch32v30x_misc.h"
#include "ch32v30x_rcc.h"
#include "ch32v30x_tim.h"

#define DAC_DMA_CHANNEL        DMA2_Channel3
#define DAC_STREAM_BUF_SAMPLES 2048U

static_assert((DAC_STREAM_BUF_SAMPLES & (DAC_STREAM_BUF_SAMPLES - 1U)) == 0U,
              "DAC stream buffer size must be a power of two");

/* The DMA target IS the producer/consumer ring: the I2S ISR writes packed
 * dual-12 words at s_write_idx, and TIM7-triggered DMA reads them out
 * cyclically. There is no rate mismatch handling — the I2S word clock and
 * TIM7's APB tick share the same crystal, so write_idx and read_idx (= the
 * DMA CNTR) walk in lockstep with a roughly constant phase offset. */
static volatile uint32_t s_dac_stream_buf[DAC_STREAM_BUF_SAMPLES];
static volatile uint32_t s_write_idx;

/* Caller must guarantee sample <= 4095. The FM path's only producer,
 * fm_q31_to_dac12, returns ((clamped + 1<<18) >> 19) where clamped is bounded
 * by (4095<<19 - 1<<18), so the dac code is provably in [0, 4095]. */
static uint32_t dac_pack_dual_12(uint16_t sample)
{
    return ((uint32_t)sample << 16) | (uint32_t)sample;
}

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

static void dac_hw_configure_direct_mode(void)
{
    DAC_InitTypeDef dac = {0};

    DAC_StructInit(&dac);
    dac.DAC_Trigger = DAC_Trigger_None;
    dac.DAC_WaveGeneration = DAC_WaveGeneration_None;
    dac.DAC_OutputBuffer = DAC_OutputBuffer_Enable;

    DAC_Init(DAC_Channel_1, &dac);
    DAC_Init(DAC_Channel_2, &dac);
    DAC_DMACmd(DAC_Channel_1, DISABLE);
    DAC_DMACmd(DAC_Channel_2, DISABLE);
    DAC_Cmd(DAC_Channel_1, ENABLE);
    DAC_Cmd(DAC_Channel_2, ENABLE);
}

static void dac_hw_configure_stream_mode(void)
{
    DAC_InitTypeDef dac = {0};

    DAC_StructInit(&dac);
    dac.DAC_Trigger = DAC_Trigger_T7_TRGO;
    dac.DAC_WaveGeneration = DAC_WaveGeneration_None;
    dac.DAC_OutputBuffer = DAC_OutputBuffer_Enable;

    DAC_Init(DAC_Channel_1, &dac);
    DAC_Init(DAC_Channel_2, &dac);
    DAC_Cmd(DAC_Channel_1, ENABLE);
    DAC_Cmd(DAC_Channel_2, ENABLE);

    /* DMA2 Channel3 is the DAC1 request. It writes RD12BDHR, which updates both DACs. */
    DAC_DMACmd(DAC_Channel_1, ENABLE);
    DAC_DMACmd(DAC_Channel_2, DISABLE);
}

static void dac_stream_timer_configure(uint32_t sample_rate_hz)
{
    uint32_t timclk = tim7_counter_clock_hz();
    uint32_t ticks = timclk / sample_rate_hz;
    if(ticks == 0U)
    {
        ticks = 1U;
    }

    uint32_t psc = 0U;
    uint32_t arr = ticks - 1U;
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
    TIM_TimeBaseInitTypeDef tim = {0};
    TIM_TimeBaseStructInit(&tim);
    tim.TIM_Period = (uint16_t)arr;
    tim.TIM_Prescaler = (uint16_t)psc;
    tim.TIM_ClockDivision = TIM_CKD_DIV1;
    tim.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM7, &tim);
    TIM_SelectOutputTrigger(TIM7, TIM_TRGOSource_Update);
    TIM_ClearFlag(TIM7, TIM_FLAG_Update);
}

static void dac_stream_dma_start(uint32_t sample_rate_hz)
{
    DMA_InitTypeDef dma = {0};

    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA2, ENABLE);

    uint32_t mid = dac_pack_dual_12(2048U);
    for(uint32_t i = 0U; i < DAC_STREAM_BUF_SAMPLES; ++i)
    {
        s_dac_stream_buf[i] = mid;
    }
    /* Start the producer a half-buffer ahead of the DMA read pointer so jitter
     * in either direction has equal margin before producer and consumer collide. */
    s_write_idx = DAC_STREAM_BUF_SAMPLES / 2U;

    DMA_DeInit(DAC_DMA_CHANNEL);

    dma.DMA_PeripheralBaseAddr = (uint32_t)&DAC->RD12BDHR;
    dma.DMA_MemoryBaseAddr = (uint32_t)s_dac_stream_buf;
    dma.DMA_DIR = DMA_DIR_PeripheralDST;
    dma.DMA_BufferSize = DAC_STREAM_BUF_SAMPLES;
    dma.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    dma.DMA_MemoryInc = DMA_MemoryInc_Enable;
    dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Word;
    dma.DMA_MemoryDataSize = DMA_MemoryDataSize_Word;
    dma.DMA_Mode = DMA_Mode_Circular;
    dma.DMA_Priority = DMA_Priority_VeryHigh;
    dma.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(DAC_DMA_CHANNEL, &dma);

    dac_stream_timer_configure(sample_rate_hz);
    dac_hw_configure_stream_mode();

    DMA_Cmd(DAC_DMA_CHANNEL, ENABLE);
    TIM_Cmd(TIM7, ENABLE);
}

static void dac_stream_dma_stop(void)
{
    TIM_Cmd(TIM7, DISABLE);
    DAC_DMACmd(DAC_Channel_1, DISABLE);
    DAC_DMACmd(DAC_Channel_2, DISABLE);
    DMA_Cmd(DAC_DMA_CHANNEL, DISABLE);
    DMA_DeInit(DAC_DMA_CHANNEL);
}

void dac_hw_init(void)
{
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_DAC, ENABLE);

    GPIO_InitTypeDef gpio = {0};
    gpio.GPIO_Pin = DAC1_OUT_GPIO_PIN | DAC2_OUT_GPIO_PIN;
    gpio.GPIO_Mode = GPIO_Mode_AIN;
    GPIO_Init(DAC1_OUT_GPIO_PORT, &gpio);

    GPIO_SetBits(DAC_AMP_SD_GPIO_PORT, DAC_AMP_SD_GPIO_PIN);
    gpio.GPIO_Pin = DAC_AMP_SD_GPIO_PIN;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_Init(DAC_AMP_SD_GPIO_PORT, &gpio);

    dac_hw_configure_direct_mode();

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

void dac_hw_stream_fm_start(uint32_t sample_rate_hz)
{
    if(sample_rate_hz == 0U)
    {
        dac_hw_stream_stop();
        return;
    }

    dac_hw_stream_stop();

    dac_stream_dma_start(sample_rate_hz);
}

void dac_hw_stream_fm_push_sample_isr(uint16_t sample)
{
    uint32_t idx = s_write_idx;
    s_dac_stream_buf[idx] = dac_pack_dual_12(sample);
    s_write_idx = (idx + 1U) & (DAC_STREAM_BUF_SAMPLES - 1U);
}

void dac_hw_stream_stop(void)
{
    dac_stream_dma_stop();
    dac_hw_configure_direct_mode();
    DAC_SetDualChannelData(DAC_Align_12b_R, 2048U, 2048U);
}
