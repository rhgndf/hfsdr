#include "i2s.h"

#include <assert.h>
#include <math.h>
#include <stddef.h>

#include "debug.h"
#include "feature/fm_audio_out/fm_audio_out.h"
#include "pinout.h"
#include "usb.h"

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
              "32-bit I2S DMA buffer must align to full stereo frames");

static volatile uint32_t s_rx_word_count = 0U;
static volatile uint16_t s_rx_dma_buf[I2S_RX_DMA_BUFFER_WORDS];
static volatile uint32_t s_i2s_reset_coincidences = 0U;
static volatile uint32_t s_i2s_coincidences_samples = 0U;
static volatile bool s_coincidence_enabled = true;


int32_t i2s_fft_sample_arr[I2S_HW_COMPLEX_SAMPLE_COUNT * 2];
static volatile uint32_t s_fft_sample_cnt = 0U;

void DMA1_Channel4_IRQHandler(void) __attribute__((interrupt));
extern void audio_usb_mic_write_isr(volatile uint16_t const *src_words, size_t word_count);

static void i2s_hw_rx_flush(void)
{
    volatile uint16_t discarded_data;
    volatile uint16_t discarded_status;

    /*
     * DATAR is only 16 bits wide in I2S mode. If RXNE or OVR survives a stop,
     * the next DMA start can consume a stale half-word and shift the stream by
     * 16 bits. Drain any unread receive data and clear OVR using the required
     * DATAR then STATR sequence from the reference manual.
     */
    do
    {
        while(SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_RXNE) != RESET)
        {
            discarded_data = SPI_I2S_ReceiveData(SPI2);
            (void)discarded_data;
        }

        if(SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_OVR) != RESET)
        {
            discarded_data = SPI_I2S_ReceiveData(SPI2);
            discarded_status = SPI2->STATR;
            (void)discarded_data;
            (void)discarded_status;
        }
    } while((SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_RXNE) != RESET) ||
            (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_OVR) != RESET));
}

static void i2s_hw_dma_irq_init(void)
{
    NVIC_InitTypeDef nvic = {0};

    nvic.NVIC_IRQChannel = I2S_RX_DMA_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 1;
    nvic.NVIC_IRQChannelSubPriority = 0;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);
}

static void i2s_hw_dma_irq_deinit(void)
{
    NVIC_InitTypeDef nvic = {0};

    nvic.NVIC_IRQChannel = I2S_RX_DMA_IRQn;
    nvic.NVIC_IRQChannelCmd = DISABLE;
    NVIC_Init(&nvic);
}

static void i2s_coincidence_detect(uint16_t const *src_words)
{
    uint32_t const *src32 = (uint32_t const *)(uintptr_t)src_words;
    uint32_t coincidences = 0;
    for(size_t i = 0; i < I2S_RX_DMA_CHUNK_WORDS / 2U; i++)
    {
        uint32_t raw = src32[i];
        uint32_t sample_32 = (raw << 16) | (raw >> 16);
        uint32_t s31 = sample_32 >> 31;
        uint32_t s30 = (sample_32 >> 30) & 1U;
        uint32_t s0  = sample_32 & 1U;
        coincidences += ((s31 ^ s30) ^ 1U) & (s31 ^ s0);
    }
    s_i2s_reset_coincidences += coincidences;
    s_i2s_coincidences_samples += I2S_RX_DMA_CHUNK_WORDS / 2;
}

static uint32_t i2s_coincidence_threshold(uint32_t samples)
{
    return (uint32_t)(1.2879f * sqrtf((float)samples));
}

static void i2s_process_buf(uint16_t const *src_words)
{
    if(s_coincidence_enabled)
    {
        i2s_coincidence_detect(src_words);
    }

    uint32_t fft_idx = s_fft_sample_cnt;
    constexpr uint32_t fft_cap = I2S_HW_COMPLEX_SAMPLE_COUNT * 2U;
    uint32_t const *src32 = (uint32_t const *)(uintptr_t)src_words;
    for(size_t i = 0; i < I2S_RX_DMA_CHUNK_WORDS / 2U; i++)
    {
        uint32_t raw = src32[i];
        uint32_t sample_32 = (raw << 16) | (raw >> 16);
        if(fft_idx < fft_cap)
        {
            i2s_fft_sample_arr[fft_idx++] = (int32_t)sample_32;
        }
    }
    s_fft_sample_cnt = fft_idx;

    s_rx_word_count += I2S_RX_DMA_CHUNK_WORDS;
    usb_hw_vendor_write_isr(src_words, I2S_RX_DMA_CHUNK_WORDS);
    audio_usb_mic_write_isr(src_words, I2S_RX_DMA_CHUNK_WORDS);
    fm_audio_out_process_i2s_words_isr(src_words, I2S_RX_DMA_CHUNK_WORDS);
}

void i2s_coincidence_disable(void)
{
    s_coincidence_enabled = false;
}

i2s_coincidence_status_t i2s_coincidence_status(void)
{
    i2s_coincidence_status_t status;
    uint32_t center;
    uint32_t threshold;

    status.coincidences = s_i2s_reset_coincidences;
    status.samples = s_i2s_coincidences_samples;
    center = status.samples / 2U;
    threshold = i2s_coincidence_threshold(status.samples);
    status.acceptable_min = (threshold < center) ? (center - threshold) : 0U;
    status.acceptable_max = center + threshold;
    if(status.acceptable_max > status.samples)
    {
        status.acceptable_max = status.samples;
    }

    return status;
}

void i2s_fft_sample_arr_reset(void)
{
    s_fft_sample_cnt = 0U;
}

bool i2s_fft_sample_arr_ready(void)
{
    return s_fft_sample_cnt >= (I2S_HW_COMPLEX_SAMPLE_COUNT * 2U);
}

static void i2s_dma_rx_start(void)
{

    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);

    SPI_I2S_DMACmd(SPI2, SPI_I2S_DMAReq_Rx, DISABLE);
    DMA_Cmd(I2S_RX_DMA_CHANNEL, DISABLE);
    DMA_DeInit(I2S_RX_DMA_CHANNEL);
    i2s_hw_rx_flush();

    DMA_InitTypeDef dma_init = {0};
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
    DMA_Cmd(I2S_RX_DMA_CHANNEL, ENABLE);
    SPI_I2S_DMACmd(SPI2, SPI_I2S_DMAReq_Rx, ENABLE);
    I2S_Cmd(SPI2, ENABLE);
}

static void i2s_dma_rx_stop(void)
{
    I2S_Cmd(SPI2, DISABLE);
    SPI_I2S_DMACmd(SPI2, SPI_I2S_DMAReq_Rx, DISABLE);
    DMA_Cmd(I2S_RX_DMA_CHANNEL, DISABLE);
    DMA_ITConfig(I2S_RX_DMA_CHANNEL, DMA_IT_HT | DMA_IT_TC | DMA_IT_TE, DISABLE);
    DMA_ClearITPendingBit(I2S_RX_DMA_GL_IT | I2S_RX_DMA_HT_IT | I2S_RX_DMA_TC_IT | I2S_RX_DMA_TE_IT);
    DMA_DeInit(I2S_RX_DMA_CHANNEL);
    i2s_hw_rx_flush();
}

static ErrorStatus i2s_hw_clock_init_24mhz(void)
{
    if(RCC_GetFlagStatus(RCC_FLAG_HSERDY) == RESET)
    {
        return NoREADY;
    }

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_AFIO, ENABLE);

    GPIO_InitTypeDef gpio = {0};
    gpio.GPIO_Pin = GPIO_Pin_8;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &gpio);

    RCC_MCOConfig(RCC_MCO_HSE);

    return READY;
}

void i2s_hw_init(void)
{
    s_rx_word_count = 0U;
    s_i2s_reset_coincidences = 0U;
    s_i2s_coincidences_samples = 0U;
    s_coincidence_enabled = true;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB | RCC_APB2Periph_GPIOC, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_SPI2, ENABLE);

    GPIO_InitTypeDef gpio_init = {0};
    gpio_init.GPIO_Pin = I2S_WS_GPIO_PIN | I2S_CK_GPIO_PIN;
    gpio_init.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    gpio_init.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &gpio_init);

    gpio_init.GPIO_Pin = I2S_SD_GPIO_PIN;
    gpio_init.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    gpio_init.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &gpio_init);

    SPI_I2S_DeInit(SPI2);
    I2S_InitTypeDef i2s_init = {0};
    i2s_init.I2S_Mode = I2S_Mode_SlaveRx;
    i2s_init.I2S_Standard = I2S_Standard_Phillips;
    i2s_init.I2S_DataFormat = I2S_DataFormat_32b;
    i2s_init.I2S_MCLKOutput = I2S_MCLKOutput_Disable;
    i2s_init.I2S_AudioFreq = 192000U;
    i2s_init.I2S_CPOL = I2S_CPOL_Low;

    I2S_Init(SPI2, &i2s_init);

    i2s_hw_dma_irq_init();

    i2s_hw_clock_init_24mhz();
}

void i2s_hw_deinit(void)
{
    s_i2s_reset_coincidences = 0U;
    s_i2s_coincidences_samples = 0U;
    i2s_dma_rx_stop();
    i2s_hw_dma_irq_deinit();
    SPI_I2S_DeInit(SPI2);

    GPIO_InitTypeDef gpio_init = {0};
    gpio_init.GPIO_Pin = I2S_WS_GPIO_PIN | I2S_CK_GPIO_PIN | I2S_SD_GPIO_PIN;
    gpio_init.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    gpio_init.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &gpio_init);

    s_rx_word_count = 0U;

    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, DISABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_SPI2, DISABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB | RCC_APB2Periph_GPIOC, DISABLE);
}

uint32_t i2s_hw_rx_word_count(void)
{
    return s_rx_word_count;
}

bool i2s_needs_reset(void)
{
    // If i2s is not bitslipped, we expect coincidences to be random with 50% probability
    // We do a two sided Z-test here
    i2s_coincidence_status_t status = i2s_coincidence_status();
    bool ret = false;
    if ((status.coincidences < status.acceptable_min) ||
        (status.coincidences > status.acceptable_max)) {
        uint32_t sample_32 = ((uint32_t)s_rx_dma_buf[0] << 16) | s_rx_dma_buf[1];
        printf("coincidences: %ld/%ld, acceptable: %ld..%ld, sample: %08lX\n",
               status.coincidences,
               status.samples,
               status.acceptable_min,
               status.acceptable_max,
               sample_32);
        ret = true;
    }
    s_i2s_coincidences_samples = 0;
    s_i2s_reset_coincidences = 0;
    return ret;
}

void i2s_hw_enable(FunctionalState state)
{
    if(state == DISABLE)
    {
        i2s_dma_rx_stop();
        return;
    }

    i2s_dma_rx_start();
}

void DMA1_Channel4_IRQHandler(void)
{
    if(DMA_GetITStatus(I2S_RX_DMA_HT_IT) != RESET)
    {
        DMA_ClearITPendingBit(I2S_RX_DMA_HT_IT);
        i2s_process_buf((uint16_t const *)&s_rx_dma_buf[0]);
    }

    if(DMA_GetITStatus(I2S_RX_DMA_TC_IT) != RESET)
    {
        DMA_ClearITPendingBit(I2S_RX_DMA_TC_IT);
        i2s_process_buf((uint16_t const *)&s_rx_dma_buf[I2S_RX_DMA_CHUNK_WORDS]);
    }

    if(DMA_GetITStatus(I2S_RX_DMA_TE_IT) != RESET)
    {
        DMA_ClearITPendingBit(I2S_RX_DMA_TE_IT);
    }
}
