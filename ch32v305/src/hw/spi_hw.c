#include "spi_hw.h"

#include "debug.h"
#include "pinout.h"

void spi_hw_init(void)
{
    GPIO_InitTypeDef gpio_init = {0};
    SPI_InitTypeDef spi_init = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB | RCC_APB2Periph_AFIO | RCC_APB2Periph_SPI1, ENABLE);

    GPIO_PinRemapConfig(GPIO_Remap_SPI1, ENABLE);

    gpio_init.GPIO_Pin = SPI1_SCL_SCK_GPIO_PIN | SPI1_SDA_MOSI_GPIO_PIN;
    gpio_init.GPIO_Mode = GPIO_Mode_AF_PP;
    gpio_init.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &gpio_init);

    SPI_StructInit(&spi_init);
    spi_init.SPI_Direction = SPI_Direction_2Lines_FullDuplex;
    spi_init.SPI_Mode = SPI_Mode_Master;
    /* Panel (ST7789): one byte per SPI transfer, high bit of each byte first. */
    spi_init.SPI_DataSize = SPI_DataSize_8b;
    spi_init.SPI_FirstBit = SPI_FirstBit_MSB;
    spi_init.SPI_CPOL = SPI_CPOL_Low;
    spi_init.SPI_CPHA = SPI_CPHA_1Edge;
    spi_init.SPI_NSS = SPI_NSS_Soft;
    spi_init.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_8;
    spi_init.SPI_CRCPolynomial = 7;

    SPI_Init(SPI1, &spi_init);
    SPI_Cmd(SPI1, ENABLE);
}

uint8_t spi_hw_transfer_u8(uint8_t tx_byte)
{
    while(SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_TXE) == RESET)
    {
    }
    SPI_I2S_SendData(SPI1, tx_byte);

    while(SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_RXNE) == RESET)
    {
    }

    return (uint8_t)SPI_I2S_ReceiveData(SPI1);
}
