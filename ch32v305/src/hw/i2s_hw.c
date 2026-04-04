#include "i2s_hw.h"

#include <assert.h>
#include <stddef.h>

#include "debug.h"
#include "pinout.h"
#include "usb_hw.h"

#include "ch32v30x_dma.h"
#include "ch32v30x_misc.h"
#include "ch32v30x_rcc.h"
#include "ch32v30x_spi.h"
#include "ch32v30x_tim.h"

/*
 * SPI2 I2S slave RX with DMA1 Channel4 circular RX.
 * DMA HT/TC interrupts count incoming words.
 * PC6 is used by TIM8_CH1 as an alternate 24 MHz clock output, so SPI2 MCK
 * is intentionally left unused.
 */
#define I2S_RX_DMA_CHANNEL           DMA1_Channel4
#define I2S_RX_DMA_IRQn              DMA1_Channel4_IRQn
#define I2S_RX_DMA_HT_IT             DMA1_IT_HT4
#define I2S_RX_DMA_TC_IT             DMA1_IT_TC4
#define I2S_RX_DMA_TE_IT             DMA1_IT_TE4
#define I2S_RX_DMA_GL_IT             DMA1_IT_GL4
#define I2S_RX_DMA_BUFFER_WORDS      512U
#define I2S_RX_FRAME_WORDS           4U
#define I2S_RX_DMA_CHUNK_WORDS       (I2S_RX_DMA_BUFFER_WORDS / 2U)
#define I2S_RX_DMA_CHUNK_BYTES       (I2S_RX_DMA_CHUNK_WORDS * sizeof(uint16_t))

static_assert((I2S_RX_DMA_BUFFER_WORDS % I2S_RX_FRAME_WORDS) == 0U,
              "24-bit I2S DMA buffer must align to full stereo frames");

static volatile uint32_t s_rx_word_count = 0U;
static volatile uint16_t s_rx_dma_buf[I2S_RX_DMA_BUFFER_WORDS];

void DMA1_Channel4_IRQHandler(void) __attribute__((interrupt));
extern void audio_usb_mic_write_isr(volatile uint16_t const *src_words, size_t word_count);

static void i2s_hw_dma_irq_init(void)
{
    NVIC_InitTypeDef nvic = {0};

    nvic.NVIC_IRQChannel = I2S_RX_DMA_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 1;
    nvic.NVIC_IRQChannelSubPriority = 0;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);
}

static void i2s_dma_rx_start(void)
{
    DMA_InitTypeDef dma_init = {0};

    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);

    DMA_DeInit(I2S_RX_DMA_CHANNEL);

    dma_init.DMA_PeripheralBaseAddr = (uint32_t)&SPI2->DATAR;
    dma_init.DMA_MemoryBaseAddr = (uint32_t)s_rx_dma_buf;
    dma_init.DMA_DIR = DMA_DIR_PeripheralSRC;
    dma_init.DMA_BufferSize = I2S_RX_DMA_BUFFER_WORDS;
    dma_init.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    dma_init.DMA_MemoryInc = DMA_MemoryInc_Enable;
    dma_init.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord;
    dma_init.DMA_MemoryDataSize = DMA_MemoryDataSize_HalfWord;
    dma_init.DMA_Mode = DMA_Mode_Circular;
    dma_init.DMA_Priority = DMA_Priority_High;
    dma_init.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(I2S_RX_DMA_CHANNEL, &dma_init);

    DMA_ClearITPendingBit(I2S_RX_DMA_GL_IT | I2S_RX_DMA_HT_IT | I2S_RX_DMA_TC_IT | I2S_RX_DMA_TE_IT);
    DMA_ITConfig(I2S_RX_DMA_CHANNEL, DMA_IT_HT | DMA_IT_TC | DMA_IT_TE, ENABLE);
    SPI_I2S_DMACmd(SPI2, SPI_I2S_DMAReq_Rx, ENABLE);
    I2S_Cmd(SPI2, ENABLE);
    DMA_Cmd(I2S_RX_DMA_CHANNEL, ENABLE);
}

static void i2s_dma_rx_stop(void)
{
    DMA_Cmd(I2S_RX_DMA_CHANNEL, DISABLE);
    SPI_I2S_DMACmd(SPI2, SPI_I2S_DMAReq_Rx, DISABLE);
    DMA_DeInit(I2S_RX_DMA_CHANNEL);
}

/* Unfortunately we wired the clock wrong, so we have to use TIM8 as a clock out instead */
static ErrorStatus i2s_hw_alt_clock_init_24mhz(void)
{
    GPIO_InitTypeDef gpio = {0};
    TIM_TimeBaseInitTypeDef tim = {0};
    TIM_OCInitTypeDef oc = {0};
    RCC_ClocksTypeDef clocks = {0};
    uint32_t tim_clk_hz;
    uint32_t period_ticks;

    RCC_GetClocksFreq(&clocks);

    if((RCC->CFGR0 & RCC_PPRE2) == RCC_PPRE2_DIV1)
    {
        tim_clk_hz = clocks.PCLK2_Frequency;
    }
    else
    {
        tim_clk_hz = clocks.PCLK2_Frequency * 2U;
    }

    if((tim_clk_hz % 24000000U) != 0U)
    {
        return NoREADY;
    }

    period_ticks = tim_clk_hz / 24000000U;
    if(period_ticks < 2U)
    {
        return NoREADY;
    }

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC | RCC_APB2Periph_AFIO | RCC_APB2Periph_TIM8, ENABLE);

    gpio.GPIO_Pin = GPIO_Pin_6;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOC, &gpio);

    TIM_DeInit(TIM8);
    tim.TIM_Prescaler = 0U;
    tim.TIM_CounterMode = TIM_CounterMode_Up;
    tim.TIM_Period = period_ticks - 1U;
    tim.TIM_ClockDivision = TIM_CKD_DIV1;
    tim.TIM_RepetitionCounter = 0U;
    TIM_TimeBaseInit(TIM8, &tim);

    oc.TIM_OCMode = TIM_OCMode_PWM1;
    oc.TIM_OutputState = TIM_OutputState_Enable;
    oc.TIM_Pulse = period_ticks / 2U;
    oc.TIM_OCPolarity = TIM_OCPolarity_High;
    TIM_OC1Init(TIM8, &oc);
    TIM_OC1PreloadConfig(TIM8, TIM_OCPreload_Enable);
    TIM_ARRPreloadConfig(TIM8, ENABLE);
    TIM_CtrlPWMOutputs(TIM8, ENABLE);
    TIM_Cmd(TIM8, ENABLE);

    return READY;
}

void i2s_hw_init(void)
{
    GPIO_InitTypeDef gpio_init = {0};
    I2S_InitTypeDef i2s_init = {0};
    
    s_rx_word_count = 0U;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB | RCC_APB2Periph_GPIOC, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_SPI2, ENABLE);

    gpio_init.GPIO_Pin = I2S_WS_GPIO_PIN | I2S_CK_GPIO_PIN;
    gpio_init.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    gpio_init.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &gpio_init);

    gpio_init.GPIO_Pin = I2S_SD_GPIO_PIN;
    gpio_init.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    gpio_init.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &gpio_init);

    SPI_I2S_DeInit(SPI2);
    i2s_init.I2S_Mode = I2S_Mode_SlaveRx;
    i2s_init.I2S_Standard = I2S_Standard_Phillips;
    i2s_init.I2S_DataFormat = I2S_DataFormat_24b;
    i2s_init.I2S_MCLKOutput = I2S_MCLKOutput_Disable;
    i2s_init.I2S_AudioFreq = 384000U;
    i2s_init.I2S_CPOL = I2S_CPOL_Low;

    I2S_Init(SPI2, &i2s_init);

    i2s_hw_dma_irq_init();

    (void)i2s_hw_alt_clock_init_24mhz();
}

uint32_t i2s_hw_rx_word_count(void)
{
    return s_rx_word_count;
}

void i2s_hw_enable(FunctionalState state)
{
    if(state == DISABLE)
    {
        i2s_dma_rx_stop();
        I2S_Cmd(SPI2, DISABLE);
        return;
    }

    i2s_dma_rx_start();
}

void DMA1_Channel4_IRQHandler(void)
{
    if(DMA_GetITStatus(I2S_RX_DMA_HT_IT) != RESET)
    {
        DMA_ClearITPendingBit(I2S_RX_DMA_HT_IT);
        s_rx_word_count += I2S_RX_DMA_CHUNK_WORDS;
        audio_usb_mic_write_isr(&s_rx_dma_buf[0], I2S_RX_DMA_CHUNK_WORDS);
        usb_hw_vendor_write_isr(&s_rx_dma_buf[0], I2S_RX_DMA_CHUNK_WORDS);
    }

    if(DMA_GetITStatus(I2S_RX_DMA_TC_IT) != RESET)
    {
        DMA_ClearITPendingBit(I2S_RX_DMA_TC_IT);
        s_rx_word_count += I2S_RX_DMA_CHUNK_WORDS;
        audio_usb_mic_write_isr(&s_rx_dma_buf[I2S_RX_DMA_CHUNK_WORDS], I2S_RX_DMA_CHUNK_WORDS);
        usb_hw_vendor_write_isr(&s_rx_dma_buf[I2S_RX_DMA_CHUNK_WORDS], I2S_RX_DMA_CHUNK_WORDS);
    }

    if(DMA_GetITStatus(I2S_RX_DMA_TE_IT) != RESET)
    {
        DMA_ClearITPendingBit(I2S_RX_DMA_TE_IT);
    }
}
