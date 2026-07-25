#include "i2s.h"

#include <assert.h>
#include <stddef.h>

#include "FreeRTOS.h"
#include "task.h"

#include "debug.h"
#include "demod/demod.h"
#include "dac.h"
#include "freertos/port_isr.h"
#include "main.h"
#include "pinout.h"
#include "usb.h"

#include "ch32v30x_dma.h"
#include "ch32v30x_gpio.h"
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
#define I2S_WS_SYNC_TIMEOUT_POLLS    100000U

static_assert((I2S_RX_DMA_BUFFER_WORDS % I2S_RX_FRAME_WORDS) == 0U,
              "32-bit I2S DMA buffer must align to full stereo frames");

static volatile uint32_t s_rx_word_count = 0U;
static volatile uint16_t s_rx_dma_buf[I2S_RX_DMA_BUFFER_WORDS];
static volatile bool s_i2s_needs_reset = false;
static volatile bool s_bitslip_detect_enabled = true;
static TaskHandle_t s_i2s_task_handle;

typedef enum
{
    I2S_HW_CLOCK_NONE = 0,
    I2S_HW_CLOCK_MCO_HSE,
    I2S_HW_CLOCK_TIM8_CH1,
} i2s_hw_clock_source_t;

int32_t i2s_fft_sample_arr[I2S_HW_COMPLEX_SAMPLE_COUNT * 2];
static volatile uint32_t s_fft_sample_cnt = 0U;

extern void audio_usb_mic_write(volatile uint16_t const *src_words, size_t word_count);

static_assert(sizeof(uintptr_t) <= sizeof(uint32_t),
              "I2S task notifications must hold a DMA buffer pointer");

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

static void i2s_bitslip_detect(volatile uint16_t const *src_words)
{
    if(s_i2s_needs_reset)
    {
        return;
    }

    volatile uint32_t const *src32 = (volatile uint32_t const *)(uintptr_t)src_words;
    for(size_t i = 0; i < I2S_RX_DMA_CHUNK_WORDS / 2U; i++)
    {
        uint32_t raw = src32[i];
        uint32_t sample_32 = (raw << 16) | (raw >> 16);
        bool should_be_ch1_mute = (i & 1U) == 0U;
        if(should_be_ch1_mute && (sample_32 != 0U))
        {
            s_i2s_needs_reset = true;
            return;
        }
    }
}

static void i2s_process_buf(volatile uint16_t *src_words)
{
    if(s_bitslip_detect_enabled)
    {
        i2s_bitslip_detect(src_words);
    }

    usb_hw_vendor_write(src_words, I2S_RX_DMA_CHUNK_WORDS);

    uint32_t fft_idx = s_fft_sample_cnt;
    constexpr uint32_t fft_cap = I2S_HW_COMPLEX_SAMPLE_COUNT * 2U;
    volatile uint32_t *src32 = (volatile uint32_t *)(uintptr_t)src_words;
    for(size_t i = 0; i < I2S_RX_DMA_CHUNK_WORDS / 2U; i++)
    {
        uint32_t raw = src32[i];
        uint32_t sample_32 = (raw << 16) | (raw >> 16);
        src32[i] = sample_32;
        if(fft_idx < fft_cap)
        {
            i2s_fft_sample_arr[fft_idx++] = (int32_t)sample_32;
        }
    }
    s_fft_sample_cnt = fft_idx;

    s_rx_word_count += I2S_RX_DMA_CHUNK_WORDS;
    audio_usb_mic_write(src_words, I2S_RX_DMA_CHUNK_WORDS);
    demod_process(src_words, I2S_RX_DMA_CHUNK_WORDS);
    dac_hw_stream_adjust_buffer();
}

void i2s_task(void *parameters)
{
    (void)parameters;
    configASSERT(s_i2s_task_handle == NULL);
    s_i2s_task_handle = xTaskGetCurrentTaskHandle();

    for(;;)
    {
        uint32_t notified_value;
        BaseType_t notified =
            xTaskNotifyWait(0U, UINT32_MAX, &notified_value, portMAX_DELAY);

        if(notified == pdTRUE)
        {
            volatile uint16_t *buffer =
                (volatile uint16_t *)(uintptr_t)notified_value;
            i2s_process_buf(buffer);
        }
    }
}

void i2s_sync_check_disable(void)
{
    s_bitslip_detect_enabled = false;
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

[[maybe_unused]] static void i2s_hw_clock_deinit_24mhz(void)
{
    RCC_MCOConfig(RCC_MCO_NoClock);

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);

    GPIO_InitTypeDef gpio = {0};
    gpio.GPIO_Pin = GPIO_Pin_8;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &gpio);
}

/* Unfortunately we wired the clock wrong on the prototype,
   so we have to use TIM8 as a clock out instead */
static ErrorStatus i2s_hw_alt_clock_init_24mhz(void)
{
    RCC_ClocksTypeDef clocks = {0};

    RCC_GetClocksFreq(&clocks);

    uint32_t tim_clk_hz;
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

    uint32_t period_ticks = tim_clk_hz / 24000000U;
    if(period_ticks < 2U)
    {
        return NoREADY;
    }

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC | RCC_APB2Periph_AFIO | RCC_APB2Periph_TIM8, ENABLE);

    GPIO_InitTypeDef gpio = {0};
    gpio.GPIO_Pin = GPIO_Pin_6;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOC, &gpio);

    TIM_DeInit(TIM8);
    TIM_TimeBaseInitTypeDef tim = {0};
    tim.TIM_Prescaler = 0U;
    tim.TIM_CounterMode = TIM_CounterMode_Up;
    tim.TIM_Period = period_ticks - 1U;
    tim.TIM_ClockDivision = TIM_CKD_DIV1;
    tim.TIM_RepetitionCounter = 0U;
    TIM_TimeBaseInit(TIM8, &tim);

    TIM_OCInitTypeDef oc = {0};
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

static void i2s_hw_alt_clock_deinit(void)
{
    TIM_Cmd(TIM8, DISABLE);
    TIM_CtrlPWMOutputs(TIM8, DISABLE);
    TIM_OC1PreloadConfig(TIM8, TIM_OCPreload_Disable);
    TIM_DeInit(TIM8);

    GPIO_InitTypeDef gpio = {0};
    gpio.GPIO_Pin = GPIO_Pin_6;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOC, &gpio);

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_TIM8, DISABLE);
}

[[maybe_unused]] static void i2s_hw_clock_deinit(void)
{
    if(get_hardware_rev() == HARDWARE_REV_V2)
    {
        i2s_hw_clock_deinit_24mhz();
    }
    else
    {
        i2s_hw_alt_clock_deinit();
    }
}

void i2s_hw_init(void)
{
    s_rx_word_count = 0U;
    s_i2s_needs_reset = false;
    s_bitslip_detect_enabled = true;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_SPI2, ENABLE);

    GPIO_InitTypeDef gpio_init = {0};
    gpio_init.GPIO_Pin = I2S_WS_GPIO_PIN | I2S_CK_GPIO_PIN | I2S_SD_GPIO_PIN;
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

    if(get_hardware_rev() == HARDWARE_REV_V2)
    {
        i2s_hw_clock_init_24mhz();
    }
    else
    {
        i2s_hw_alt_clock_init_24mhz();
    }
}

void i2s_hw_deinit(void)
{
    s_i2s_needs_reset = false;
    i2s_dma_rx_stop();
    i2s_hw_dma_irq_deinit();
    SPI_I2S_DeInit(SPI2);

    GPIO_InitTypeDef gpio_init = {0};
    gpio_init.GPIO_Pin = I2S_WS_GPIO_PIN | I2S_CK_GPIO_PIN | I2S_SD_GPIO_PIN;
    gpio_init.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    gpio_init.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &gpio_init);

    s_rx_word_count = 0U;

    RCC_APB1PeriphResetCmd(RCC_APB1Periph_SPI2, ENABLE);
    Delay_Ms(1);
    RCC_APB1PeriphResetCmd(RCC_APB1Periph_SPI2, DISABLE);

    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, DISABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_SPI2, DISABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, DISABLE);
}

uint32_t i2s_hw_rx_word_count(void)
{
    return s_rx_word_count;
}

bool i2s_needs_reset(void)
{
    bool ret = s_i2s_needs_reset;
    if(ret)
    {
        volatile uint32_t const *samples = (volatile uint32_t const *)(uintptr_t)s_rx_dma_buf;
        uint32_t sample_320 = samples[0];
        uint32_t sample_321 = samples[1];
        uint32_t sample_322 = samples[2];
        uint32_t sample_323 = samples[3];
        printf("muted-ch1 pattern failure, sample: %08lX %08lX %08lX %08lX\n", sample_320, sample_321, sample_322, sample_323);
    }
    s_i2s_needs_reset = false;
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

PORT_ISR_BODY(DMA1_Channel4_IRQHandler)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    if(DMA_GetITStatus(I2S_RX_DMA_HT_IT) != RESET)
    {
        DMA_ClearITPendingBit(I2S_RX_DMA_HT_IT);
        (void)xTaskNotifyFromISR(s_i2s_task_handle,
                                 (uint32_t)(uintptr_t)&s_rx_dma_buf[0],
                                 eSetValueWithOverwrite,
                                 &higher_priority_task_woken);
    }

    if(DMA_GetITStatus(I2S_RX_DMA_TC_IT) != RESET)
    {
        DMA_ClearITPendingBit(I2S_RX_DMA_TC_IT);
        (void)xTaskNotifyFromISR(
            s_i2s_task_handle,
            (uint32_t)(uintptr_t)&s_rx_dma_buf[I2S_RX_DMA_CHUNK_WORDS],
            eSetValueWithOverwrite,
            &higher_priority_task_woken);
    }

    if(DMA_GetITStatus(I2S_RX_DMA_TE_IT) != RESET)
    {
        DMA_ClearITPendingBit(I2S_RX_DMA_TE_IT);
    }

    portYIELD_FROM_ISR(higher_priority_task_woken);
}
