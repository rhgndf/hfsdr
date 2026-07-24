#include "spi.h"

#include "FreeRTOS.h"
#include "task.h"
#include "freertos/port_isr.h"

#include "debug.h"
#include "main.h"
#include "pinout.h"

#include "ch32v30x_dma.h"
#include "ch32v30x_misc.h"
#include "ch32v30x_rcc.h"
#include "ch32v30x_spi.h"

#define SPI3_TX_DMA_CHANNEL            DMA2_Channel2
#define SPI3_TX_DMA_IRQn               DMA2_Channel2_IRQn
#define SPI3_TX_DMA_FLAG_GL            DMA2_FLAG_GL2
#define SPI3_TX_DMA_FLAG_TC            DMA2_FLAG_TC2
#define SPI3_TX_DMA_FLAG_TE            DMA2_FLAG_TE2
#define SPI3_TX_DMA_IT_GL              DMA2_IT_GL2
#define SPI3_TX_DMA_IT_TC              DMA2_IT_TC2
#define SPI3_TX_DMA_IT_TE              DMA2_IT_TE2
#define SPI3_TX_DMA_MAX_LEN            65535U

static uint16_t s_spi3_data_size = SPI_DataSize_8b;
static uint16_t s_spi3_tx_repeat_word = 0U;

static void spi_hw_set_data_size(uint16_t data_size)
{
    if(s_spi3_data_size == data_size)
    {
        return;
    }

    while(SPI_I2S_GetFlagStatus(SPI3, SPI_I2S_FLAG_BSY) != RESET)
    {
    }

    SPI_Cmd(SPI3, DISABLE);
    SPI_DataSizeConfig(SPI3, data_size);
    SPI_Cmd(SPI3, ENABLE);
    s_spi3_data_size = data_size;
}

void spi_hw_wait_dma(void)
{

    bool const dma_in_progress =
        ((SPI3_TX_DMA_CHANNEL->CFGR & DMA_CFGR1_EN) != 0U) &&
        (DMA_GetCurrDataCounter(SPI3_TX_DMA_CHANNEL) > 0U);

    if(dma_in_progress)
    {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }

    /* DMA completion means the last word reached DATAR, not necessarily the
     * wire.  The remaining BSY interval is at most one SPI frame. */
    while(SPI_I2S_GetFlagStatus(SPI3, SPI_I2S_FLAG_BSY) != RESET)
    {
    }

    SPI_I2S_DMACmd(SPI3, SPI_I2S_DMAReq_Tx, DISABLE);
    DMA_ITConfig(SPI3_TX_DMA_CHANNEL, DMA_IT_TC | DMA_IT_TE, DISABLE);
    DMA_Cmd(SPI3_TX_DMA_CHANNEL, DISABLE);
    DMA_ClearFlag(SPI3_TX_DMA_FLAG_GL | SPI3_TX_DMA_FLAG_TC | SPI3_TX_DMA_FLAG_TE);
    NVIC_ClearPendingIRQ(SPI3_TX_DMA_IRQn);
}

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
    spi_init.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_2;
    spi_init.SPI_CRCPolynomial = 7;

    SPI_Init(SPI3, &spi_init);
    SPI_Cmd(SPI3, ENABLE);

    SPI_I2S_DMACmd(SPI3, SPI_I2S_DMAReq_Tx, DISABLE);
    DMA_Cmd(SPI3_TX_DMA_CHANNEL, DISABLE);
    DMA_DeInit(SPI3_TX_DMA_CHANNEL);
    DMA_ClearFlag(SPI3_TX_DMA_FLAG_GL | SPI3_TX_DMA_FLAG_TC | SPI3_TX_DMA_FLAG_TE);
    NVIC_ClearPendingIRQ(SPI3_TX_DMA_IRQn);

    NVIC_InitTypeDef dma_irq = {0};
    dma_irq.NVIC_IRQChannel = SPI3_TX_DMA_IRQn;
    dma_irq.NVIC_IRQChannelPreemptionPriority = 2U;
    dma_irq.NVIC_IRQChannelSubPriority = 0U;
    dma_irq.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&dma_irq);

    s_spi3_data_size = SPI_DataSize_8b;
}

/* SPI3 is configured 1Line_Tx (transmit only), so there is nothing to receive.
 * The return value is always 0 and exists only to match the historical signature;
 * callers should ignore it. */
uint8_t spi_hw_transfer_u8(uint8_t tx_byte)
{
    spi_hw_set_data_size(SPI_DataSize_8b);
    while(SPI_I2S_GetFlagStatus(SPI3, SPI_I2S_FLAG_TXE) == RESET)
    {
    }
    SPI_I2S_SendData(SPI3, tx_byte);
    return 0U;
}

static void spi_hw_transfer_dma_chunk(const uint8_t *tx_buf, uint16_t len)
{
    if((tx_buf == NULL) || (len == 0U))
    {
        return;
    }
    spi_hw_set_data_size(SPI_DataSize_8b);

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

    DMA_ITConfig(SPI3_TX_DMA_CHANNEL, DMA_IT_TC | DMA_IT_TE, ENABLE);
    DMA_Cmd(SPI3_TX_DMA_CHANNEL, ENABLE);
    SPI_I2S_DMACmd(SPI3, SPI_I2S_DMAReq_Tx, ENABLE);
}

static void spi_hw_transfer_dma_repeat_u16_chunk(uint16_t tx_word, uint16_t count)
{
    if(count == 0U)
    {
        return;
    }
    spi_hw_set_data_size(SPI_DataSize_16b);
    s_spi3_tx_repeat_word = tx_word;

    SPI_I2S_DMACmd(SPI3, SPI_I2S_DMAReq_Tx, DISABLE);
    DMA_Cmd(SPI3_TX_DMA_CHANNEL, DISABLE);
    DMA_DeInit(SPI3_TX_DMA_CHANNEL);
    DMA_ClearFlag(SPI3_TX_DMA_FLAG_GL | SPI3_TX_DMA_FLAG_TC | SPI3_TX_DMA_FLAG_TE);

    DMA_InitTypeDef dma_init = {0};
    dma_init.DMA_PeripheralBaseAddr = (uint32_t)&SPI3->DATAR;
    dma_init.DMA_MemoryBaseAddr = (uint32_t)&s_spi3_tx_repeat_word;
    dma_init.DMA_DIR = DMA_DIR_PeripheralDST;
    dma_init.DMA_BufferSize = count;
    dma_init.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    dma_init.DMA_MemoryInc = DMA_MemoryInc_Disable;
    dma_init.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord;
    dma_init.DMA_MemoryDataSize = DMA_MemoryDataSize_HalfWord;
    dma_init.DMA_Mode = DMA_Mode_Normal;
    dma_init.DMA_Priority = DMA_Priority_High;
    dma_init.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(SPI3_TX_DMA_CHANNEL, &dma_init);

    DMA_ITConfig(SPI3_TX_DMA_CHANNEL, DMA_IT_TC | DMA_IT_TE, ENABLE);
    DMA_Cmd(SPI3_TX_DMA_CHANNEL, ENABLE);
    SPI_I2S_DMACmd(SPI3, SPI_I2S_DMAReq_Tx, ENABLE);
}

static void spi_hw_transfer_dma_u16_chunk(const uint16_t *tx_buf, uint16_t count)
{
    if((tx_buf == NULL) || (count == 0U))
    {
        return;
    }
    spi_hw_set_data_size(SPI_DataSize_16b);

    SPI_I2S_DMACmd(SPI3, SPI_I2S_DMAReq_Tx, DISABLE);
    DMA_Cmd(SPI3_TX_DMA_CHANNEL, DISABLE);
    DMA_DeInit(SPI3_TX_DMA_CHANNEL);
    DMA_ClearFlag(SPI3_TX_DMA_FLAG_GL | SPI3_TX_DMA_FLAG_TC | SPI3_TX_DMA_FLAG_TE);

    DMA_InitTypeDef dma_init = {0};
    dma_init.DMA_PeripheralBaseAddr = (uint32_t)&SPI3->DATAR;
    dma_init.DMA_MemoryBaseAddr = (uint32_t)tx_buf;
    dma_init.DMA_DIR = DMA_DIR_PeripheralDST;
    dma_init.DMA_BufferSize = count;
    dma_init.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    dma_init.DMA_MemoryInc = DMA_MemoryInc_Enable;
    dma_init.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord;
    dma_init.DMA_MemoryDataSize = DMA_MemoryDataSize_HalfWord;
    dma_init.DMA_Mode = DMA_Mode_Normal;
    dma_init.DMA_Priority = DMA_Priority_High;
    dma_init.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(SPI3_TX_DMA_CHANNEL, &dma_init);

    DMA_ITConfig(SPI3_TX_DMA_CHANNEL, DMA_IT_TC | DMA_IT_TE, ENABLE);
    DMA_Cmd(SPI3_TX_DMA_CHANNEL, ENABLE);
    SPI_I2S_DMACmd(SPI3, SPI_I2S_DMAReq_Tx, ENABLE);
}

PORT_ISR_BODY(DMA2_Channel2_IRQHandler)
{
    BaseType_t higher_priority_task_woken = pdFALSE;
    bool const transfer_ended =
        (DMA_GetITStatus(SPI3_TX_DMA_IT_TC) != RESET) ||
        (DMA_GetITStatus(SPI3_TX_DMA_IT_TE) != RESET);

    DMA_ClearITPendingBit(SPI3_TX_DMA_IT_GL |
                          SPI3_TX_DMA_IT_TC |
                          SPI3_TX_DMA_IT_TE);

    if(transfer_ended)
    {
        vTaskNotifyGiveFromISR(g_application_task_handle,
                               &higher_priority_task_woken);
    }

    portYIELD_FROM_ISR(higher_priority_task_woken);
}

void spi_hw_transfer_dma(const uint8_t *tx_buf, size_t len)
{
    if((tx_buf == NULL) || (len == 0U))
    {
        return;
    }

    while(len > 0U)
    {
        spi_hw_wait_dma();
        uint16_t chunk_len = (len > SPI3_TX_DMA_MAX_LEN) ? SPI3_TX_DMA_MAX_LEN : (uint16_t)len;
        spi_hw_transfer_dma_chunk(tx_buf, chunk_len);
        tx_buf += chunk_len;
        len -= chunk_len;
    }
}

void spi_hw_transfer_dma_repeat_u16(uint16_t tx_word, size_t count)
{
    while(count > 0U)
    {
        spi_hw_wait_dma();
        uint16_t chunk_count = (count > SPI3_TX_DMA_MAX_LEN) ? SPI3_TX_DMA_MAX_LEN : (uint16_t)count;
        spi_hw_transfer_dma_repeat_u16_chunk(tx_word, chunk_count);
        count -= chunk_count;
    }
}

void spi_hw_transfer_dma_u16(const uint16_t *tx_buf, size_t count)
{
    if((tx_buf == NULL) || (count == 0U))
    {
        return;
    }

    while(count > 0U)
    {
        spi_hw_wait_dma();
        uint16_t chunk_count = (count > SPI3_TX_DMA_MAX_LEN) ? SPI3_TX_DMA_MAX_LEN : (uint16_t)count;
        spi_hw_transfer_dma_u16_chunk(tx_buf, chunk_count);
        tx_buf += chunk_count;
        count -= chunk_count;
    }
}
