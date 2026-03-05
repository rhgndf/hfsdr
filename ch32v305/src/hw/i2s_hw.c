#include "i2s_hw.h"

#include "debug.h"
#include "pinout.h"

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
    I2S_Cmd(SPI2, state);
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
    while(SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_RXNE) == RESET)
    {
    }
    return SPI_I2S_ReceiveData(SPI2);
}

ErrorStatus i2s_hw_try_receive_u16(uint16_t *sample)
{
    if(sample == 0)
    {
        return NoREADY;
    }

    if(SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_RXNE) == RESET)
    {
        return NoREADY;
    }

    *sample = SPI_I2S_ReceiveData(SPI2);
    return READY;
}
