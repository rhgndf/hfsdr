#include "spi.h"

#include "debug.h"
#include "pinout.h"

#include "ch32v30x_dma.h"
#include "ch32v30x_rcc.h"
#include "ch32v30x_spi.h"

#define SPI3_TX_DMA_CHANNEL DMA2_Channel2
#define SPI3_TX_DMA_FLAG_GL DMA2_FLAG_GL2
#define SPI3_TX_DMA_FLAG_TC DMA2_FLAG_TC2
#define SPI3_TX_DMA_FLAG_TE DMA2_FLAG_TE2
#define SPI3_TX_DMA_MAX_LEN 65535U

void spi_hw_init(void)
{
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB | RCC_APB2Periph_AFIO, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_SPI3, ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA2, ENABLE);

    SPI_Cmd(SPI3, DISABLE);
    SPI_I2S_DeInit(SPI3);

    GPIO_InitTypeDef gpio_init = {0};
    gpio_init.GPIO_Pin = SPI3_SCL_SCK_GPIO_PIN | SPI3_SDA_MOSI_GPIO_PIN;
    gpio_init.GPIO_Mode = GPIO_Mode_AF_PP;
    gpio_init.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &gpio_init);

    SPI_InitTypeDef spi_init = {0};
    SPI_StructInit(&spi_init);
    spi_init.SPI_Direction = SPI_Direction_1Line_Tx;
    spi_init.SPI_Mode = SPI_Mode_Master;
    /* Panel (ST7789): one byte per SPI transfer, high bit of each byte first. */
    spi_init.SPI_DataSize = SPI_DataSize_8b;
    spi_init.SPI_FirstBit = SPI_FirstBit_MSB;
    spi_init.SPI_CPOL = SPI_CPOL_Low;
    spi_init.SPI_CPHA = SPI_CPHA_1Edge;
    spi_init.SPI_NSS = SPI_NSS_Soft;
    spi_init.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_8;
    spi_init.SPI_CRCPolynomial = 7;

    SPI_Init(SPI3, &spi_init);
    SPI_Cmd(SPI3, ENABLE);

    SPI_I2S_DMACmd(SPI3, SPI_I2S_DMAReq_Tx, DISABLE);
    DMA_Cmd(SPI3_TX_DMA_CHANNEL, DISABLE);
    DMA_DeInit(SPI3_TX_DMA_CHANNEL);
    DMA_ClearFlag(SPI3_TX_DMA_FLAG_GL | SPI3_TX_DMA_FLAG_TC | SPI3_TX_DMA_FLAG_TE);
}

uint8_t spi_hw_transfer_u8(uint8_t tx_byte)
{
    while(SPI_I2S_GetFlagStatus(SPI3, SPI_I2S_FLAG_TXE) == RESET)
    {
    }
    SPI_I2S_SendData(SPI3, tx_byte);

    while(SPI_I2S_GetFlagStatus(SPI3, SPI_I2S_FLAG_BSY) != RESET)
    {
    }

    return 0U;
}

static void spi_hw_transfer_dma_chunk(const uint8_t *tx_buf, uint16_t len)
{
    if((tx_buf == NULL) || (len == 0U))
    {
        return;
    }

    while(SPI_I2S_GetFlagStatus(SPI3, SPI_I2S_FLAG_BSY) != RESET)
    {
    }

    SPI_I2S_DMACmd(SPI3, SPI_I2S_DMAReq_Tx, DISABLE);
    DMA_Cmd(SPI3_TX_DMA_CHANNEL, DISABLE);
    DMA_DeInit(SPI3_TX_DMA_CHANNEL);
    DMA_ClearFlag(SPI3_TX_DMA_FLAG_GL | SPI3_TX_DMA_FLAG_TC | SPI3_TX_DMA_FLAG_TE);

    DMA_InitTypeDef dma_init = {0};
    dma_init.DMA_PeripheralBaseAddr = (uint32_t)&SPI3->DATAR;
    dma_init.DMA_MemoryBaseAddr = (uint32_t)tx_buf;
    dma_init.DMA_DIR = DMA_DIR_PeripheralDST;
    dma_init.DMA_BufferSize = len;
    dma_init.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    dma_init.DMA_MemoryInc = DMA_MemoryInc_Enable;
    dma_init.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    dma_init.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
    dma_init.DMA_Mode = DMA_Mode_Normal;
    dma_init.DMA_Priority = DMA_Priority_High;
    dma_init.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(SPI3_TX_DMA_CHANNEL, &dma_init);

    SPI_I2S_DMACmd(SPI3, SPI_I2S_DMAReq_Tx, ENABLE);
    DMA_Cmd(SPI3_TX_DMA_CHANNEL, ENABLE);

    while(DMA_GetFlagStatus(SPI3_TX_DMA_FLAG_TC) == RESET)
    {
        if(DMA_GetFlagStatus(SPI3_TX_DMA_FLAG_TE) != RESET)
        {
            break;
        }
    }

    DMA_Cmd(SPI3_TX_DMA_CHANNEL, DISABLE);
    SPI_I2S_DMACmd(SPI3, SPI_I2S_DMAReq_Tx, DISABLE);
    DMA_ClearFlag(SPI3_TX_DMA_FLAG_GL | SPI3_TX_DMA_FLAG_TC | SPI3_TX_DMA_FLAG_TE);

    while(SPI_I2S_GetFlagStatus(SPI3, SPI_I2S_FLAG_BSY) != RESET)
    {
    }
}

void spi_hw_transfer_dma(const uint8_t *tx_buf, size_t len)
{
    while(len > 0U)
    {
        uint16_t chunk_len = (len > SPI3_TX_DMA_MAX_LEN) ? SPI3_TX_DMA_MAX_LEN : (uint16_t)len;
        spi_hw_transfer_dma_chunk(tx_buf, chunk_len);
        tx_buf += chunk_len;
        len -= chunk_len;
    }
}
