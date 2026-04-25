#include "dac_hw.h"

#include <assert.h>
#include <stddef.h>

#include "hw/pinout.h"

#include "ch32v30x_dac.h"
#include "ch32v30x_dma.h"
#include "ch32v30x_gpio.h"
#include "ch32v30x_misc.h"
#include "ch32v30x_rcc.h"
#include "ch32v30x_tim.h"

#define DAC_STREAM_MODE_IDLE   0U
#define DAC_STREAM_MODE_SINE   1U
#define DAC_STREAM_MODE_NOISE  2U
#define DAC_STREAM_MODE_FM     3U

#define DAC_DMA_CHANNEL        DMA2_Channel3
#define DAC_DMA_IRQn           DMA2_Channel3_IRQn
#define DAC_DMA_HT_IT          DMA2_IT_HT3
#define DAC_DMA_TC_IT          DMA2_IT_TC3
#define DAC_DMA_TE_IT          DMA2_IT_TE3
#define DAC_DMA_GL_IT          DMA2_IT_GL3
#define DAC_STREAM_BUF_SAMPLES 512U
#define DAC_STREAM_CHUNK_SAMPLES (DAC_STREAM_BUF_SAMPLES / 2U)
#define DAC_FM_RING_SAMPLES    2048U

static_assert((DAC_STREAM_BUF_SAMPLES % 2U) == 0U,
              "DAC stream buffer must split evenly for DMA HT/TC refills");
static_assert((DAC_FM_RING_SAMPLES & (DAC_FM_RING_SAMPLES - 1U)) == 0U,
              "FM DAC ring size must be a power of two");

/* 256 samples, centered at 2048, amplitude ~1900 (fits 0..4095). */
static const uint16_t s_sine_lut[256] = {
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

static uint16_t sq_low;
static uint16_t sq_high;
static uint8_t sq_phase;

static uint8_t s_stream_mode = DAC_STREAM_MODE_IDLE;
static uint32_t s_phase_acc;
static uint32_t s_phase_inc;
static uint32_t s_rng32;
static volatile uint32_t s_tx_frame_count;
static uint32_t s_dac_stream_buf[DAC_STREAM_BUF_SAMPLES];
static volatile uint16_t s_fm_ring[DAC_FM_RING_SAMPLES];
static volatile uint32_t s_fm_write_idx;
static volatile uint32_t s_fm_read_idx;
static uint16_t s_fm_hold_sample = 2048U;

void DMA2_Channel3_IRQHandler(void) __attribute__((interrupt));

static uint32_t dac_pack_dual_12(uint16_t sample)
{
    uint32_t clamped = sample;

    if(clamped > 4095U)
    {
        clamped = 4095U;
    }

    return (clamped << 16) | clamped;
}

static void dac_fm_ring_reset(void)
{
    s_fm_write_idx = 0U;
    s_fm_read_idx = 0U;
    s_fm_hold_sample = 2048U;
}

static uint16_t dac_stream_next_sample(void)
{
    uint32_t idx;
    uint32_t x;
    uint32_t read_idx;

    if(s_stream_mode == DAC_STREAM_MODE_NOISE)
    {
        x = s_rng32;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        s_rng32 = x;
        return (uint16_t)((x >> 20) & 0x0FFFU);
    }

    if(s_stream_mode == DAC_STREAM_MODE_SINE)
    {
        s_phase_acc += s_phase_inc;
        idx = s_phase_acc >> 24;
        return s_sine_lut[idx];
    }

    if(s_stream_mode == DAC_STREAM_MODE_FM)
    {
        read_idx = s_fm_read_idx;
        if(read_idx != s_fm_write_idx)
        {
            uint16_t sample = s_fm_ring[read_idx & (DAC_FM_RING_SAMPLES - 1U)];
            s_fm_read_idx = read_idx + 1U;
            s_fm_hold_sample = sample;
        }
        return s_fm_hold_sample;
    }

    return 2048U;
}

static void dac_stream_fill(uint32_t *dst, size_t n)
{
    for(size_t i = 0U; i < n; ++i)
    {
        dst[i] = dac_pack_dual_12(dac_stream_next_sample());
    }
}

static uint32_t tim6_counter_clock_hz(void)
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

static uint32_t tim7_counter_clock_hz(void)
{
    return tim6_counter_clock_hz();
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
    DAC_DMACmd(DAC_Channel_1, ENABLE);
    DAC_DMACmd(DAC_Channel_2, ENABLE);
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

static void dac_stream_dma_irq_init(void)
{
    NVIC_InitTypeDef nvic = {0};

    nvic.NVIC_IRQChannel = DAC_DMA_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 2;
    nvic.NVIC_IRQChannelSubPriority = 1;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);
}

static void dac_stream_dma_start(uint32_t sample_rate_hz)
{
    DMA_InitTypeDef dma = {0};

    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA2, ENABLE);

    dac_stream_fill(s_dac_stream_buf, DAC_STREAM_BUF_SAMPLES);

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

    DMA_ClearITPendingBit(DAC_DMA_GL_IT | DAC_DMA_HT_IT | DAC_DMA_TC_IT | DAC_DMA_TE_IT);
    DMA_ITConfig(DAC_DMA_CHANNEL, DMA_IT_HT | DMA_IT_TC | DMA_IT_TE, ENABLE);

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
    DMA_ClearITPendingBit(DAC_DMA_GL_IT | DAC_DMA_HT_IT | DAC_DMA_TC_IT | DAC_DMA_TE_IT);
}

void dac_hw_init(void)
{
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_DAC, ENABLE);

    GPIO_InitTypeDef gpio = {0};
    gpio.GPIO_Pin = DAC1_OUT_GPIO_PIN | DAC2_OUT_GPIO_PIN;
    gpio.GPIO_Mode = GPIO_Mode_AIN;
    GPIO_Init(DAC1_OUT_GPIO_PORT, &gpio);

    s_tx_frame_count = 0U;
    dac_stream_dma_irq_init();
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

    dac_hw_square_wave_stop();
    dac_hw_stream_stop();

    dac_fm_ring_reset();
    s_stream_mode = DAC_STREAM_MODE_FM;
    dac_stream_dma_start(sample_rate_hz);
}

void dac_hw_stream_fm_push_sample_isr(uint16_t sample)
{
    uint32_t write_idx = s_fm_write_idx;

    if(sample > 4095U)
    {
        sample = 4095U;
    }

    if((write_idx - s_fm_read_idx) >= DAC_FM_RING_SAMPLES)
    {
        return;
    }

    s_fm_ring[write_idx & (DAC_FM_RING_SAMPLES - 1U)] = sample;
    s_fm_write_idx = write_idx + 1U;
}

void dac_hw_stream_stop(void)
{
    s_stream_mode = DAC_STREAM_MODE_IDLE;
    dac_fm_ring_reset();
    dac_stream_dma_stop();
    dac_hw_configure_direct_mode();
    DAC_SetDualChannelData(DAC_Align_12b_R, 2048U, 2048U);
}

uint32_t dac_hw_tx_frame_count(void)
{
    return s_tx_frame_count;
}

void dac_hw_stream_noise_start(uint32_t sample_rate_hz)
{
    if(sample_rate_hz == 0U)
    {
        dac_hw_stream_stop();
        return;
    }

    dac_hw_square_wave_stop();
    dac_hw_stream_stop();

    s_rng32 = 0xA341316CU;
    s_stream_mode = DAC_STREAM_MODE_NOISE;
    dac_stream_dma_start(sample_rate_hz);
}

void dac_hw_stream_sine_start(uint32_t sine_freq_hz, uint32_t sample_rate_hz)
{
    if(sample_rate_hz == 0U || sine_freq_hz == 0U)
    {
        dac_hw_stream_stop();
        return;
    }

    dac_hw_square_wave_stop();
    dac_hw_stream_stop();

    s_phase_acc = 0U;
    s_phase_inc = (uint32_t)(((uint64_t)sine_freq_hz << 32) / (uint64_t)sample_rate_hz);
    s_stream_mode = DAC_STREAM_MODE_SINE;
    dac_stream_dma_start(sample_rate_hz);
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
    dac_hw_stream_stop();
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

    uint32_t timclk = tim6_counter_clock_hz();
    uint32_t ticks_half = timclk / (2U * freq_hz);
    if(ticks_half == 0U)
    {
        ticks_half = 1U;
    }

    uint32_t psc = 0U;
    uint32_t arr = ticks_half - 1U;
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
    TIM_TimeBaseInitTypeDef tim = {0};
    TIM_TimeBaseStructInit(&tim);
    tim.TIM_Period = (uint16_t)arr;
    tim.TIM_Prescaler = (uint16_t)psc;
    tim.TIM_ClockDivision = TIM_CKD_DIV1;
    tim.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM6, &tim);
    TIM_ClearFlag(TIM6, TIM_FLAG_Update);
    TIM_ITConfig(TIM6, TIM_IT_Update, ENABLE);

    NVIC_InitTypeDef nvic = {0};
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

void DMA2_Channel3_IRQHandler(void)
{
    if(DMA_GetITStatus(DAC_DMA_HT_IT) != RESET)
    {
        DMA_ClearITPendingBit(DAC_DMA_HT_IT);
        dac_stream_fill(s_dac_stream_buf, DAC_STREAM_CHUNK_SAMPLES);
        s_tx_frame_count += DAC_STREAM_CHUNK_SAMPLES;
    }

    if(DMA_GetITStatus(DAC_DMA_TC_IT) != RESET)
    {
        DMA_ClearITPendingBit(DAC_DMA_TC_IT);
        dac_stream_fill(&s_dac_stream_buf[DAC_STREAM_CHUNK_SAMPLES], DAC_STREAM_CHUNK_SAMPLES);
        s_tx_frame_count += DAC_STREAM_CHUNK_SAMPLES;
    }

    if(DMA_GetITStatus(DAC_DMA_TE_IT) != RESET)
    {
        DMA_ClearITPendingBit(DAC_DMA_TE_IT);
    }
}
