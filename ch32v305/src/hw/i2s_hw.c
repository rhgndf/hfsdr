#include "i2s_hw.h"

#include "debug.h"
#include "pinout.h"

#include "ch32v30x_dma.h"
#include "ch32v30x_rcc.h"
#include "ch32v30x_spi.h"

/*
 * SPI2 I2S master RX: DMA1 channel 4, peripheral -> memory, circular.
 * (WCH EVT: SPI1 RX=Ch2 / TX=Ch3; I2S example SPI2 TX=DMA1 Ch5 — SPI2 RX is Ch4.)
 */
#define I2S_DMA_BUF_LEN 512U

static uint16_t s_dma_buf[I2S_DMA_BUF_LEN];
static volatile uint32_t s_rx_rd = 0U;

#define I2S_RX_DMA_CH DMA1_Channel4

static uint32_t i2s_dma_rx_write_idx(void)
{
    uint16_t left = DMA_GetCurrDataCounter(I2S_RX_DMA_CH);

    return (uint32_t)((I2S_DMA_BUF_LEN - (uint32_t)left) % I2S_DMA_BUF_LEN);
}

static void i2s_dma_rx_start(void)
{
    DMA_InitTypeDef dma = {0};

    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);

    DMA_DeInit(I2S_RX_DMA_CH);

    s_rx_rd = 0U;

    dma.DMA_PeripheralBaseAddr = (uint32_t)&SPI2->DATAR;
    dma.DMA_MemoryBaseAddr = (uint32_t)s_dma_buf;
    dma.DMA_DIR = DMA_DIR_PeripheralSRC;
    dma.DMA_BufferSize = (uint32_t)I2S_DMA_BUF_LEN;
    dma.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    dma.DMA_MemoryInc = DMA_MemoryInc_Enable;
    dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord;
    dma.DMA_MemoryDataSize = DMA_MemoryDataSize_HalfWord;
    dma.DMA_Mode = DMA_Mode_Circular;
    dma.DMA_Priority = DMA_Priority_High;
    dma.DMA_M2M = DMA_M2M_Disable;

    DMA_Init(I2S_RX_DMA_CH, &dma);

    SPI_I2S_DMACmd(SPI2, SPI_I2S_DMAReq_Rx, ENABLE);

    I2S_Cmd(SPI2, ENABLE);

    DMA_Cmd(I2S_RX_DMA_CH, ENABLE);
}

static void i2s_dma_rx_stop(void)
{
    DMA_Cmd(I2S_RX_DMA_CH, DISABLE);
    SPI_I2S_DMACmd(SPI2, SPI_I2S_DMAReq_Rx, DISABLE);
    DMA_DeInit(I2S_RX_DMA_CH);
}

void i2s_hw_init(void)
{
    GPIO_InitTypeDef gpio_init = {0};
    I2S_InitTypeDef i2s_init = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB | RCC_APB2Periph_GPIOC, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_SPI2, ENABLE);

    gpio_init.GPIO_Pin = I2S_WS_GPIO_PIN | I2S_CK_GPIO_PIN;
    gpio_init.GPIO_Mode = GPIO_Mode_AF_PP;
    gpio_init.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &gpio_init);

    gpio_init.GPIO_Pin = I2S_MCK_GPIO_PIN;
    gpio_init.GPIO_Mode = GPIO_Mode_AF_PP;
    gpio_init.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(I2S_MCK_GPIO_PORT, &gpio_init);

    gpio_init.GPIO_Pin = I2S_SD_GPIO_PIN;
    gpio_init.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    gpio_init.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &gpio_init);

    SPI_I2S_DeInit(SPI2);
    i2s_init.I2S_Mode = I2S_Mode_MasterRx;
    i2s_init.I2S_Standard = I2S_Standard_Phillips;
    i2s_init.I2S_DataFormat = I2S_DataFormat_16b;
    i2s_init.I2S_MCLKOutput = I2S_MCLKOutput_Enable;
    i2s_init.I2S_AudioFreq = I2S_AudioFreq_48k;
    i2s_init.I2S_CPOL = I2S_CPOL_Low;

    I2S_Init(SPI2, &i2s_init);
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

void i2s_hw_send_u16(uint16_t sample)
{
    while(SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_TXE) == RESET)
    {
    }
    SPI_I2S_SendData(SPI2, sample);
}

uint16_t i2s_hw_receive_u16(void)
{
    uint16_t w;

    while(i2s_hw_try_receive_u16(&w) != READY)
    {
    }

    return w;
}

ErrorStatus i2s_hw_try_receive_u16(uint16_t *sample)
{
    uint32_t wr;
    uint32_t rd;

    if(sample == 0)
    {
        return NoREADY;
    }

    wr = i2s_dma_rx_write_idx();
    rd = s_rx_rd;

    if(wr == rd)
    {
        return NoREADY;
    }

    *sample = s_dma_buf[rd];
    s_rx_rd = (rd + 1U) % I2S_DMA_BUF_LEN;

    return READY;
}

void i2s_hw_receive_burst_blocking(uint16_t *buf, size_t n)
{
    size_t i;

    for(i = 0U; i < n; ++i)
    {
        buf[i] = i2s_hw_receive_u16();
    }
}

size_t i2s_hw_receive_drain_try(uint16_t *buf, size_t max_n)
{
    size_t i;

    if(buf == NULL || max_n == 0U)
    {
        return 0U;
    }

    for(i = 0U; i < max_n; ++i)
    {
        if(i2s_hw_try_receive_u16(&buf[i]) != READY)
        {
            break;
        }
    }

    return i;
}
