#include "test/spi_gpio_pins.h"

#include "hw/pinout.h"
#include "hw/spi_hw.h"

#include "ch32v30x_spi.h"

void spi_gpio_pins_enable(void)
{
    GPIO_InitTypeDef g = {0};

    SPI_Cmd(SPI1, DISABLE);
    SPI_I2S_DeInit(SPI1);

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB | RCC_APB2Periph_GPIOC | RCC_APB2Periph_GPIOD | RCC_APB2Periph_AFIO,
                           ENABLE);

    g.GPIO_Mode = GPIO_Mode_Out_PP;
    g.GPIO_Speed = GPIO_Speed_50MHz;

    g.GPIO_Pin = SPI1_SCL_SCK_GPIO_PIN | SPI1_SDA_MOSI_GPIO_PIN;
    GPIO_Init(SPI1_SCL_SCK_GPIO_PORT, &g);

    g.GPIO_Pin = ST7789_CS_GPIO_PIN;
    GPIO_Init(ST7789_CS_GPIO_PORT, &g);

    g.GPIO_Pin = I2C_RS_GPIO_PIN;
    GPIO_Init(I2C_RS_GPIO_PORT, &g);

    g.GPIO_Pin = ST7789_RST_GPIO_PIN;
    GPIO_Init(ST7789_RST_GPIO_PORT, &g);

    /* Idle-ish defaults: SCK/MOSI low, CS high (inactive), RS high, RST low (panel run — active-high reset). */
    GPIO_WriteBit(SPI1_SCL_SCK_GPIO_PORT, SPI1_SCL_SCK_GPIO_PIN, Bit_RESET);
    GPIO_WriteBit(SPI1_SDA_MOSI_GPIO_PORT, SPI1_SDA_MOSI_GPIO_PIN, Bit_RESET);
    GPIO_WriteBit(ST7789_CS_GPIO_PORT, ST7789_CS_GPIO_PIN, Bit_SET);
    GPIO_WriteBit(I2C_RS_GPIO_PORT, I2C_RS_GPIO_PIN, Bit_SET);
    GPIO_WriteBit(ST7789_RST_GPIO_PORT, ST7789_RST_GPIO_PIN, Bit_RESET);
}

void spi_gpio_pins_restore_hw_spi(void)
{
    spi_hw_init();
}


void spi_gpio_sck_write(BitAction level)
{
    GPIO_WriteBit(SPI1_SCL_SCK_GPIO_PORT, SPI1_SCL_SCK_GPIO_PIN, level);
}

void spi_gpio_mosi_write(BitAction level)
{
    GPIO_WriteBit(SPI1_SDA_MOSI_GPIO_PORT, SPI1_SDA_MOSI_GPIO_PIN, level);
}

void spi_gpio_cs_write(BitAction level)
{
    GPIO_WriteBit(ST7789_CS_GPIO_PORT, ST7789_CS_GPIO_PIN, level);
}

void spi_gpio_rs_write(BitAction level)
{
    GPIO_WriteBit(I2C_RS_GPIO_PORT, I2C_RS_GPIO_PIN, level);
}

void spi_gpio_rst_write(BitAction level)
{
    GPIO_WriteBit(ST7789_RST_GPIO_PORT, ST7789_RST_GPIO_PIN, level);
}

void spi_gpio_pins_all_on(void)
{
    spi_gpio_sck_write(Bit_SET);
    spi_gpio_mosi_write(Bit_SET);
    spi_gpio_cs_write(Bit_SET);
    spi_gpio_rs_write(Bit_SET);
    spi_gpio_rst_write(Bit_SET);
}

void spi_gpio_sck_toggle(void)
{
    GPIO_WriteBit(SPI1_SCL_SCK_GPIO_PORT, SPI1_SCL_SCK_GPIO_PIN,
                  GPIO_ReadOutputDataBit(SPI1_SCL_SCK_GPIO_PORT, SPI1_SCL_SCK_GPIO_PIN) == Bit_RESET ? Bit_SET : Bit_RESET);
}

void spi_gpio_mosi_toggle(void)
{
    GPIO_WriteBit(SPI1_SDA_MOSI_GPIO_PORT, SPI1_SDA_MOSI_GPIO_PIN,
                  GPIO_ReadOutputDataBit(SPI1_SDA_MOSI_GPIO_PORT, SPI1_SDA_MOSI_GPIO_PIN) == Bit_RESET ? Bit_SET
                                                                                                       : Bit_RESET);
}

void spi_gpio_cs_toggle(void)
{
    GPIO_WriteBit(ST7789_CS_GPIO_PORT, ST7789_CS_GPIO_PIN,
                  GPIO_ReadOutputDataBit(ST7789_CS_GPIO_PORT, ST7789_CS_GPIO_PIN) == Bit_RESET ? Bit_SET : Bit_RESET);
}

void spi_gpio_rs_toggle(void)
{
    GPIO_WriteBit(I2C_RS_GPIO_PORT, I2C_RS_GPIO_PIN,
                  GPIO_ReadOutputDataBit(I2C_RS_GPIO_PORT, I2C_RS_GPIO_PIN) == Bit_RESET ? Bit_SET : Bit_RESET);
}
