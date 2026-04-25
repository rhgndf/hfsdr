#include "i2c_hw.h"

extern "C" {
#include "debug.h"
#include "pinout.h"
}

namespace
{

constexpr uint32_t I2C_HW_TIMEOUT = 100000U;
constexpr uint8_t I2C_ADDRESS_MAX_7BIT = 0x7FU;
constexpr uint32_t I2C_CLOCK_SPEED_HZ = 1000000U;
constexpr uint8_t I2C_OWN_ADDRESS = 0x32U;

void i2c_hw_recover_bus() noexcept
{
    I2C_GenerateSTOP(I2C2, ENABLE);
    I2C_ClearFlag(I2C2, I2C_FLAG_AF | I2C_FLAG_ARLO | I2C_FLAG_BERR);
    I2C_SoftwareResetCmd(I2C2, ENABLE);
    I2C_SoftwareResetCmd(I2C2, DISABLE);
    I2C_Cmd(I2C2, ENABLE);
    I2C_AcknowledgeConfig(I2C2, ENABLE);
}

class I2CTransactionGuard
{
public:
    I2CTransactionGuard() = default;

    ~I2CTransactionGuard() noexcept
    {
        if(m_active)
        {
            i2c_hw_recover_bus();
        }
    }

    void finish() noexcept
    {
        I2C_GenerateSTOP(I2C2, ENABLE);
        m_active = false;
    }

    void dismiss() noexcept
    {
        m_active = false;
    }

private:
    bool m_active = true;
};

class I2CAckGuard
{
public:
    I2CAckGuard() = default;

    ~I2CAckGuard() noexcept
    {
        if(m_restore_ack)
        {
            I2C_AcknowledgeConfig(I2C2, ENABLE);
        }
    }

    void disable() noexcept
    {
        I2C_AcknowledgeConfig(I2C2, DISABLE);
        m_restore_ack = true;
    }

private:
    bool m_restore_ack = false;
};

ErrorStatus i2c_hw_wait_event(uint32_t event)
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

} // namespace

extern "C" ErrorStatus i2c_hw_scan_bus_at(uint8_t addr_7bit)
{
    uint32_t timeout = I2C_HW_TIMEOUT;
    I2CTransactionGuard guard;

    if(addr_7bit > I2C_ADDRESS_MAX_7BIT)
    {
        return NoREADY;
    }

    I2C_ClearFlag(I2C2, I2C_FLAG_AF | I2C_FLAG_ARLO | I2C_FLAG_BERR);

    I2C_GenerateSTART(I2C2, ENABLE);
    if(i2c_hw_wait_event(I2C_EVENT_MASTER_MODE_SELECT) != READY)
    {
        return NoREADY;
    }

    I2C_Send7bitAddress(I2C2, static_cast<uint8_t>(addr_7bit << 1), I2C_Direction_Transmitter);

    while(timeout-- > 0U)
    {
        if(I2C_CheckEvent(I2C2, I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED) == READY)
        {
            guard.finish();
            return READY;
        }

        if(I2C_GetFlagStatus(I2C2, I2C_FLAG_AF) == SET)
        {
            return NoREADY;
        }
    }

    return NoREADY;
}

extern "C" void i2c_hw_init(void)
{
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB | RCC_APB2Periph_GPIOC, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_I2C2, ENABLE);

    GPIO_InitTypeDef gpio_init = {0};
    gpio_init.GPIO_Pin = I2C_SCL_GPIO_PIN | I2C_SDA_GPIO_PIN;
    gpio_init.GPIO_Mode = GPIO_Mode_AF_OD;
    gpio_init.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &gpio_init);

    gpio_init.GPIO_Pin = ST7789_RS_GPIO_PIN;
    gpio_init.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio_init.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(ST7789_RS_GPIO_PORT, &gpio_init);
    GPIO_SetBits(ST7789_RS_GPIO_PORT, ST7789_RS_GPIO_PIN);

    I2C_InitTypeDef i2c_init = {0};
    I2C_StructInit(&i2c_init);
    i2c_init.I2C_ClockSpeed = I2C_CLOCK_SPEED_HZ;
    i2c_init.I2C_Mode = I2C_Mode_I2C;
    i2c_init.I2C_DutyCycle = I2C_DutyCycle_2;
    i2c_init.I2C_OwnAddress1 = I2C_OWN_ADDRESS;
    i2c_init.I2C_Ack = I2C_Ack_Enable;
    i2c_init.I2C_AcknowledgedAddress = I2C_AcknowledgedAddress_7bit;

    I2C_Init(I2C2, &i2c_init);
    I2C_Cmd(I2C2, ENABLE);
    I2C_AcknowledgeConfig(I2C2, ENABLE);
}

extern "C" ErrorStatus i2c_hw_write_register(uint8_t addr_7bit, uint8_t reg, uint8_t value)
{
    I2CTransactionGuard guard;

    I2C_GenerateSTART(I2C2, ENABLE);
    if(i2c_hw_wait_event(I2C_EVENT_MASTER_MODE_SELECT) != READY)
    {
        return NoREADY;
    }

    I2C_Send7bitAddress(I2C2, static_cast<uint8_t>(addr_7bit << 1), I2C_Direction_Transmitter);
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

    guard.finish();
    return READY;
}

extern "C" ErrorStatus i2c_hw_write_register_burst(uint8_t addr_7bit, uint8_t reg, const uint8_t *data, size_t len)
{
    I2CTransactionGuard guard;

    if(data == nullptr)
    {
        return NoREADY;
    }

    I2C_GenerateSTART(I2C2, ENABLE);
    if(i2c_hw_wait_event(I2C_EVENT_MASTER_MODE_SELECT) != READY)
    {
        return NoREADY;
    }

    I2C_Send7bitAddress(I2C2, static_cast<uint8_t>(addr_7bit << 1), I2C_Direction_Transmitter);
    if(i2c_hw_wait_event(I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED) != READY)
    {
        return NoREADY;
    }

    I2C_SendData(I2C2, reg);
    if(i2c_hw_wait_event(I2C_EVENT_MASTER_BYTE_TRANSMITTED) != READY)
    {
        return NoREADY;
    }

    for(size_t i = 0U; i < len; ++i)
    {
        I2C_SendData(I2C2, data[i]);
        if(i2c_hw_wait_event(I2C_EVENT_MASTER_BYTE_TRANSMITTED) != READY)
        {
            return NoREADY;
        }
    }

    guard.finish();
    return READY;
}

extern "C" ErrorStatus i2c_hw_read_register(uint8_t addr_7bit, uint8_t reg, uint8_t *value)
{
    I2CTransactionGuard guard;
    I2CAckGuard ack_guard;

    if(value == nullptr)
    {
        return NoREADY;
    }

    I2C_GenerateSTART(I2C2, ENABLE);
    if(i2c_hw_wait_event(I2C_EVENT_MASTER_MODE_SELECT) != READY)
    {
        return NoREADY;
    }

    I2C_Send7bitAddress(I2C2, static_cast<uint8_t>(addr_7bit << 1), I2C_Direction_Transmitter);
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

    I2C_Send7bitAddress(I2C2, static_cast<uint8_t>(addr_7bit << 1), I2C_Direction_Receiver);
    if(i2c_hw_wait_event(I2C_EVENT_MASTER_RECEIVER_MODE_SELECTED) != READY)
    {
        return NoREADY;
    }

    ack_guard.disable();
    guard.finish();
    if(i2c_hw_wait_event(I2C_EVENT_MASTER_BYTE_RECEIVED) != READY)
    {
        return NoREADY;
    }

    *value = I2C_ReceiveData(I2C2);
    guard.dismiss();
    return READY;
}
