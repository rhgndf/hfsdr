#include "i2c_hw.h"

#include "debug.h"
#include "pinout.h"

#define I2C_HW_TIMEOUT  100000U

static void i2c_hw_recover_bus(void)
{
    I2C_GenerateSTOP(I2C2, ENABLE);
    I2C_ClearFlag(I2C2, I2C_FLAG_AF | I2C_FLAG_ARLO | I2C_FLAG_BERR);
    I2C_SoftwareResetCmd(I2C2, ENABLE);
    I2C_SoftwareResetCmd(I2C2, DISABLE);
    I2C_Cmd(I2C2, ENABLE);
    I2C_AcknowledgeConfig(I2C2, ENABLE);
}

static ErrorStatus i2c_hw_wait_event(uint32_t event)
{
    uint32_t timeout = I2C_HW_TIMEOUT;
    while(I2C_CheckEvent(I2C2, event) != READY)
    {
        if(timeout-- == 0U)
        {
            return NoREADY;
        }
    }

    return READY;
}

ErrorStatus i2c_hw_scan_bus_at(uint8_t addr_7bit)
{
    uint32_t timeout = I2C_HW_TIMEOUT;

    if(addr_7bit > 0x7FU)
    {
        return NoREADY;
    }

    I2C_ClearFlag(I2C2, I2C_FLAG_AF | I2C_FLAG_ARLO | I2C_FLAG_BERR);

    I2C_GenerateSTART(I2C2, ENABLE);
    if(i2c_hw_wait_event(I2C_EVENT_MASTER_MODE_SELECT) != READY)
    {
        i2c_hw_recover_bus();
        return NoREADY;
    }

    I2C_Send7bitAddress(I2C2, (uint8_t)(addr_7bit << 1), I2C_Direction_Transmitter);

    while(timeout-- > 0U)
    {
        if(I2C_CheckEvent(I2C2, I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED) == READY)
        {
            I2C_GenerateSTOP(I2C2, ENABLE);
            return READY;
        }

        if(I2C_GetFlagStatus(I2C2, I2C_FLAG_AF) == SET)
        {
            i2c_hw_recover_bus();
            return NoREADY;
        }
    }

    i2c_hw_recover_bus();
    return NoREADY;
}

void i2c_hw_init(void)
{
    GPIO_InitTypeDef gpio_init = {0};
    I2C_InitTypeDef i2c_init = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB | RCC_APB2Periph_GPIOC, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_I2C2, ENABLE);

    gpio_init.GPIO_Pin = I2C_SCL_GPIO_PIN | I2C_SDA_GPIO_PIN;
    gpio_init.GPIO_Mode = GPIO_Mode_AF_OD;
    gpio_init.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &gpio_init);

    gpio_init.GPIO_Pin = I2C_RS_GPIO_PIN;
    gpio_init.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio_init.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(I2C_RS_GPIO_PORT, &gpio_init);
    GPIO_SetBits(I2C_RS_GPIO_PORT, I2C_RS_GPIO_PIN);

    I2C_StructInit(&i2c_init);
    i2c_init.I2C_ClockSpeed = 400000;
    i2c_init.I2C_Mode = I2C_Mode_I2C;
    i2c_init.I2C_DutyCycle = I2C_DutyCycle_2;
    i2c_init.I2C_OwnAddress1 = 0x32;
    i2c_init.I2C_Ack = I2C_Ack_Enable;
    i2c_init.I2C_AcknowledgedAddress = I2C_AcknowledgedAddress_7bit;

    I2C_Init(I2C2, &i2c_init);
    I2C_Cmd(I2C2, ENABLE);
    I2C_AcknowledgeConfig(I2C2, ENABLE);
}

ErrorStatus i2c_hw_write_register(uint8_t addr_7bit, uint8_t reg, uint8_t value)
{
    I2C_GenerateSTART(I2C2, ENABLE);
    if(i2c_hw_wait_event(I2C_EVENT_MASTER_MODE_SELECT) != READY)
    {
        return NoREADY;
    }

    I2C_Send7bitAddress(I2C2, (uint8_t)(addr_7bit << 1), I2C_Direction_Transmitter);
    if(i2c_hw_wait_event(I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED) != READY)
    {
        return NoREADY;
    }

    I2C_SendData(I2C2, reg);
    if(i2c_hw_wait_event(I2C_EVENT_MASTER_BYTE_TRANSMITTED) != READY)
    {
        return NoREADY;
    }

    I2C_SendData(I2C2, value);
    if(i2c_hw_wait_event(I2C_EVENT_MASTER_BYTE_TRANSMITTED) != READY)
    {
        return NoREADY;
    }

    I2C_GenerateSTOP(I2C2, ENABLE);
    return READY;
}

ErrorStatus i2c_hw_read_register(uint8_t addr_7bit, uint8_t reg, uint8_t *value)
{
    if(value == 0)
    {
        return NoREADY;
    }

    I2C_GenerateSTART(I2C2, ENABLE);
    if(i2c_hw_wait_event(I2C_EVENT_MASTER_MODE_SELECT) != READY)
    {
        return NoREADY;
    }

    I2C_Send7bitAddress(I2C2, (uint8_t)(addr_7bit << 1), I2C_Direction_Transmitter);
    if(i2c_hw_wait_event(I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED) != READY)
    {
        return NoREADY;
    }

    I2C_SendData(I2C2, reg);
    if(i2c_hw_wait_event(I2C_EVENT_MASTER_BYTE_TRANSMITTED) != READY)
    {
        return NoREADY;
    }

    I2C_GenerateSTART(I2C2, ENABLE);
    if(i2c_hw_wait_event(I2C_EVENT_MASTER_MODE_SELECT) != READY)
    {
        return NoREADY;
    }

    I2C_Send7bitAddress(I2C2, (uint8_t)(addr_7bit << 1), I2C_Direction_Receiver);
    if(i2c_hw_wait_event(I2C_EVENT_MASTER_RECEIVER_MODE_SELECTED) != READY)
    {
        return NoREADY;
    }

    I2C_AcknowledgeConfig(I2C2, DISABLE);
    I2C_GenerateSTOP(I2C2, ENABLE);
    if(i2c_hw_wait_event(I2C_EVENT_MASTER_BYTE_RECEIVED) != READY)
    {
        I2C_AcknowledgeConfig(I2C2, ENABLE);
        return NoREADY;
    }

    *value = I2C_ReceiveData(I2C2);
    I2C_AcknowledgeConfig(I2C2, ENABLE);

    return READY;
}
