#include "spi_manual.h"

#include "hw/pinout.h"
#include "hw/spi_hw.h"

#include "ch32v30x.h"

void spi_manual_init(void)
{
    GPIO_InitTypeDef g = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC | RCC_APB2Periph_GPIOD, ENABLE);

    g.GPIO_Mode = GPIO_Mode_Out_PP;
    g.GPIO_Speed = GPIO_Speed_50MHz;

    g.GPIO_Pin = I2C_RS_GPIO_PIN;
    GPIO_Init(I2C_RS_GPIO_PORT, &g);

    g.GPIO_Pin = ST7789_CS_GPIO_PIN;
    GPIO_Init(ST7789_CS_GPIO_PORT, &g);

    g.GPIO_Pin = ST7789_RST_GPIO_PIN;
    GPIO_Init(ST7789_RST_GPIO_PORT, &g);

    GPIO_WriteBit(ST7789_CS_GPIO_PORT, ST7789_CS_GPIO_PIN, Bit_SET);
    GPIO_WriteBit(I2C_RS_GPIO_PORT, I2C_RS_GPIO_PIN, Bit_SET);
    /* Active-high RST: low = panel running */
    GPIO_WriteBit(ST7789_RST_GPIO_PORT, ST7789_RST_GPIO_PIN, Bit_RESET);

    spi_hw_init();
}

void spi_manual_cs_begin(void)
{
    GPIO_WriteBit(ST7789_CS_GPIO_PORT, ST7789_CS_GPIO_PIN, Bit_RESET);
}

void spi_manual_cs_end(void)
{
    GPIO_WriteBit(ST7789_CS_GPIO_PORT, ST7789_CS_GPIO_PIN, Bit_SET);
}

void spi_manual_rs_cmd(void)
{
    GPIO_WriteBit(I2C_RS_GPIO_PORT, I2C_RS_GPIO_PIN, Bit_RESET);
}

void spi_manual_rs_data(void)
{
    GPIO_WriteBit(I2C_RS_GPIO_PORT, I2C_RS_GPIO_PIN, Bit_SET);
}

uint8_t spi_manual_transfer_u8(uint8_t tx)
{
    return spi_hw_transfer_u8(tx);
}
