#include "i2c.h"

#include "utils/utils.h"

#include <mutex>

extern "C" {
#include "FreeRTOS.h"
#include "task.h"

#include "freertos/port_isr.h"

#include "ch32v30x_misc.h"
#include "debug.h"
#include "pinout.h"
}

namespace
{

constexpr uint8_t I2C_ADDRESS_MAX_7BIT = 0x7FU;
constexpr uint32_t I2C_CLOCK_SPEED_HZ = 1000000U;
constexpr uint8_t I2C_OWN_ADDRESS = 0x32U;
constexpr size_t I2C_WRITE_BUFFER_SIZE = 16U;
constexpr UBaseType_t I2C_NOTIFICATION_INDEX = 1U;
constexpr TickType_t I2C_TRANSACTION_TIMEOUT_TICKS = pdMS_TO_TICKS(5);
constexpr uint32_t I2C_RECOVERY_HALF_PERIOD_US = 5U;
constexpr uint32_t I2C_RECOVERY_SCL_RELEASE_TIMEOUT_US = 100U;
constexpr uint8_t I2C_RECOVERY_CLOCK_PULSES = 9U;
constexpr uint16_t I2C_INTERRUPT_BITS =
    I2C_CTLR2_ITBUFEN | I2C_CTLR2_ITEVTEN | I2C_CTLR2_ITERREN;
constexpr uint16_t I2C_ERROR_BITS =
    I2C_STAR1_BERR |
    I2C_STAR1_ARLO |
    I2C_STAR1_AF |
    I2C_STAR1_OVR |
    I2C_STAR1_PECERR |
    I2C_STAR1_TIMEOUT |
    I2C_STAR1_SMBALERT;

static_assert(I2C_TRANSACTION_TIMEOUT_TICKS > 0U);
static_assert(I2C_WRITE_BUFFER_SIZE <= UINT8_MAX);
static_assert(configTASK_NOTIFICATION_ARRAY_ENTRIES > I2C_NOTIFICATION_INDEX);

enum class I2CPhase : uint8_t
{
    Idle,
    AddressWrite,
    Transmit,
    AddressRead,
    Receive,
    Complete,
};

struct I2CState
{
    TaskHandle_t waiting_task;
    ErrorStatus result;
    I2CPhase phase;
    uint8_t addr;

    uint8_t write_buf[I2C_WRITE_BUFFER_SIZE];
    uint8_t write_len;
    uint8_t write_pos;

    uint8_t *read_buf;
    size_t read_len;
    size_t read_pos;
};

volatile I2CState s_state = {};
FreeRTOSLock s_lock;

void i2c_set_buffer_interrupt(bool enable) noexcept
{
    if(enable)
    {
        I2C2->CTLR2 |= I2C_CTLR2_ITBUFEN;
    }
    else
    {
        I2C2->CTLR2 &= static_cast<uint16_t>(~I2C_CTLR2_ITBUFEN);
    }
}

void i2c_disable_interrupts() noexcept
{
    I2C2->CTLR2 &= static_cast<uint16_t>(~I2C_INTERRUPT_BITS);
}

void i2c_enable_interrupts() noexcept
{
    I2C2->CTLR2 |= I2C_INTERRUPT_BITS;
}

void i2c_clear_error_flags() noexcept
{
    I2C2->STAR1 = static_cast<uint16_t>(~I2C_ERROR_BITS);
}

bool i2c_clear_stop_flag(uint16_t star1) noexcept
{
    if((star1 & I2C_STAR1_STOPF) == 0U)
    {
        return false;
    }

    /* EVT4: STAR1 has been read; a subsequent CTLR1 write clears STOPF. */
    uint16_t const ctlr1 = I2C2->CTLR1;
    I2C2->CTLR1 = ctlr1;
    return true;
}

void i2c_configure_peripheral() noexcept
{
    i2c_disable_interrupts();

    I2C_InitTypeDef i2c_init{};
    I2C_StructInit(&i2c_init);
    i2c_init.I2C_ClockSpeed = I2C_CLOCK_SPEED_HZ;
    i2c_init.I2C_Mode = I2C_Mode_I2C;
    i2c_init.I2C_DutyCycle = I2C_DutyCycle_16_9;
    i2c_init.I2C_OwnAddress1 = I2C_OWN_ADDRESS;
    i2c_init.I2C_Ack = I2C_Ack_Enable;
    i2c_init.I2C_AcknowledgedAddress = I2C_AcknowledgedAddress_7bit;

    I2C_Init(I2C2, &i2c_init);
    I2C_Cmd(I2C2, ENABLE);
    I2C_StretchClockCmd(I2C2, ENABLE);
    I2C_NACKPositionConfig(I2C2, I2C_NACKPosition_Current);
    I2C_AcknowledgeConfig(I2C2, ENABLE);
    i2c_clear_error_flags();
}

void i2c_configure_pins(GPIOMode_TypeDef mode) noexcept
{
    GPIO_InitTypeDef gpio_init{};
    gpio_init.GPIO_Pin = I2C_SCL_GPIO_PIN | I2C_SDA_GPIO_PIN;
    gpio_init.GPIO_Mode = mode;
    gpio_init.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(I2C_SCL_GPIO_PORT, &gpio_init);
}

bool i2c_release_scl() noexcept
{
    GPIO_WriteBit(I2C_SCL_GPIO_PORT, I2C_SCL_GPIO_PIN, Bit_SET);

    for(uint32_t elapsed_us = 0U;
        elapsed_us < I2C_RECOVERY_SCL_RELEASE_TIMEOUT_US;
        ++elapsed_us)
    {
        if(GPIO_ReadInputDataBit(I2C_SCL_GPIO_PORT,
                                 I2C_SCL_GPIO_PIN) != Bit_RESET)
        {
            return true;
        }
        Delay_Us(1U);
    }

    return false;
}

bool i2c_wait_bus_idle() noexcept
{
    TickType_t const start = xTaskGetTickCount();

    for(;;)
    {
        if(((I2C2->CTLR1 & I2C_CTLR1_STOP) == 0U) &&
           ((I2C2->STAR2 & I2C_STAR2_BUSY) == 0U))
        {
            return true;
        }

        if((xTaskGetTickCount() - start) >= I2C_TRANSACTION_TIMEOUT_TICKS)
        {
            return false;
        }
    }
}

bool i2c_recover_bus() noexcept
{
    i2c_disable_interrupts();
    I2C_Cmd(I2C2, DISABLE);
    NVIC_ClearPendingIRQ(I2C2_EV_IRQn);
    NVIC_ClearPendingIRQ(I2C2_ER_IRQn);

    /*
     * Preload both open-drain output latches high before GPIO takes control,
     * then release SDA. This avoids introducing a low glitch while changing
     * PB10/PB11 away from their alternate functions.
     */
    GPIO_WriteBit(I2C_SCL_GPIO_PORT, I2C_SCL_GPIO_PIN, Bit_SET);
    GPIO_WriteBit(I2C_SDA_GPIO_PORT, I2C_SDA_GPIO_PIN, Bit_SET);
    i2c_configure_pins(GPIO_Mode_Out_OD);
    GPIO_WriteBit(I2C_SDA_GPIO_PORT, I2C_SDA_GPIO_PIN, Bit_SET);
    Delay_Us(I2C_RECOVERY_HALF_PERIOD_US);

    for(uint8_t pulse = 0U;
        (pulse < I2C_RECOVERY_CLOCK_PULSES) &&
        (GPIO_ReadInputDataBit(I2C_SDA_GPIO_PORT,
                               I2C_SDA_GPIO_PIN) == Bit_RESET);
        ++pulse)
    {
        GPIO_WriteBit(I2C_SCL_GPIO_PORT, I2C_SCL_GPIO_PIN, Bit_RESET);
        Delay_Us(I2C_RECOVERY_HALF_PERIOD_US);
        if(!i2c_release_scl())
        {
            break;
        }
        Delay_Us(I2C_RECOVERY_HALF_PERIOD_US);
    }

    /* Generate a GPIO STOP: SDA low, release SCL high, then release SDA. */
    GPIO_WriteBit(I2C_SDA_GPIO_PORT, I2C_SDA_GPIO_PIN, Bit_RESET);
    Delay_Us(I2C_RECOVERY_HALF_PERIOD_US);
    (void)i2c_release_scl();
    Delay_Us(I2C_RECOVERY_HALF_PERIOD_US);
    GPIO_WriteBit(I2C_SDA_GPIO_PORT, I2C_SDA_GPIO_PIN, Bit_SET);
    Delay_Us(I2C_RECOVERY_HALF_PERIOD_US);

    i2c_configure_pins(GPIO_Mode_AF_OD);
    I2C_SoftwareResetCmd(I2C2, ENABLE);
    I2C_SoftwareResetCmd(I2C2, DISABLE);
    i2c_configure_peripheral();

    NVIC_ClearPendingIRQ(I2C2_EV_IRQn);
    NVIC_ClearPendingIRQ(I2C2_ER_IRQn);
    return i2c_wait_bus_idle();
}

bool i2c_phase_is_active(I2CPhase phase) noexcept
{
    switch(phase)
    {
        case I2CPhase::Idle:
        case I2CPhase::Complete:
            return false;

        default:
            return true;
    }
}

void i2c_complete_from_isr(ErrorStatus result,
                           BaseType_t *higher_priority_task_woken) noexcept
{
    i2c_disable_interrupts();
    s_state.result = result;
    s_state.phase = I2CPhase::Complete;

    TaskHandle_t const waiting_task = s_state.waiting_task;
    if(waiting_task != nullptr)
    {
        vTaskNotifyGiveIndexedFromISR(waiting_task,
                                      I2C_NOTIFICATION_INDEX,
                                      higher_priority_task_woken);
    }
}

void i2c_fail_from_isr(BaseType_t *higher_priority_task_woken) noexcept
{
    I2C_GenerateSTOP(I2C2, ENABLE);
    i2c_complete_from_isr(NoREADY, higher_priority_task_woken);
}

bool i2c_send_next_byte() noexcept
{
    uint8_t const pos = s_state.write_pos;
    uint8_t const len = s_state.write_len;
    if(pos >= len)
    {
        return false;
    }

    I2C_SendData(I2C2, s_state.write_buf[pos]);
    uint8_t const next_pos = static_cast<uint8_t>(pos + 1U);
    s_state.write_pos = next_pos;
    if(next_pos == len)
    {
        /*
         * TxE/EVT8 feeds intermediate bytes. After the final DATAR write,
         * BTF/EVT8_2 must own STOP or repeated-START generation.
         */
        i2c_set_buffer_interrupt(false);
    }
    return true;
}

bool i2c_receive_next_byte() noexcept
{
    size_t const pos = s_state.read_pos;
    size_t const len = s_state.read_len;
    uint8_t *const buffer = s_state.read_buf;
    if((buffer == nullptr) || (pos >= len))
    {
        return false;
    }

    buffer[pos] = I2C_ReceiveData(I2C2);
    s_state.read_pos = pos + 1U;
    return true;
}

size_t i2c_read_remaining() noexcept
{
    size_t const pos = s_state.read_pos;
    size_t const len = s_state.read_len;
    return (pos <= len) ? (len - pos) : 0U;
}

void i2c_handle_address_from_isr(BaseType_t *higher_priority_task_woken) noexcept
{
    I2CPhase const phase = s_state.phase;

    switch(phase)
    {
        case I2CPhase::AddressWrite:
        {
            /* STAR1 was read by the event handler; STAR2 completes the ADDR
             * clearing sequence and releases the stretched clock. */
            (void)I2C2->STAR2;

            if(s_state.write_len != 0U)
            {
                s_state.phase = I2CPhase::Transmit;
                if(!i2c_send_next_byte())
                {
                    i2c_fail_from_isr(higher_priority_task_woken);
                }
                return;
            }

            if(s_state.read_len != 0U)
            {
                s_state.phase = I2CPhase::AddressRead;
                I2C_GenerateSTART(I2C2, ENABLE);
                return;
            }

            /* Address-only transaction used by i2c_hw_scan_bus_at(). */
            I2C_GenerateSTOP(I2C2, ENABLE);
            i2c_complete_from_isr(READY, higher_priority_task_woken);
            return;
        }

        case I2CPhase::AddressRead:
            break;

        default:
            (void)I2C2->STAR2;
            i2c_fail_from_isr(higher_priority_task_woken);
            return;
    }

    size_t const remaining = i2c_read_remaining();
    if((remaining == 0U) || (s_state.read_buf == nullptr))
    {
        (void)I2C2->STAR2;
        i2c_fail_from_isr(higher_priority_task_woken);
        return;
    }

    s_state.phase = I2CPhase::Receive;

    switch(remaining)
    {
        case 1U:
        {
            /*
             * EVT6_1: NACK and STOP must be programmed adjacent to clearing
             * ADDR. Keep this sequence indivisible with respect to nested
             * higher-priority interrupts.
             */
            UBaseType_t const saved_mie = portSET_INTERRUPT_MASK_FROM_ISR();
            I2C_NACKPositionConfig(I2C2, I2C_NACKPosition_Current);
            I2C_AcknowledgeConfig(I2C2, DISABLE);
            (void)I2C2->STAR2;
            I2C_GenerateSTOP(I2C2, ENABLE);
            portCLEAR_INTERRUPT_MASK_FROM_ISR(saved_mie);
            i2c_set_buffer_interrupt(true);
            return;
        }

        case 2U:
        {
            /*
             * POS must be selected before ADDR is cleared; ACK is then
             * cleared immediately so the second byte is NACKed. BTF exposes
             * both bytes together.
             */
            UBaseType_t const saved_mie = portSET_INTERRUPT_MASK_FROM_ISR();
            I2C_NACKPositionConfig(I2C2, I2C_NACKPosition_Next);
            (void)I2C2->STAR2;
            I2C_AcknowledgeConfig(I2C2, DISABLE);
            i2c_set_buffer_interrupt(false);
            portCLEAR_INTERRUPT_MASK_FROM_ISR(saved_mie);
            return;
        }

        default:
            I2C_NACKPositionConfig(I2C2, I2C_NACKPosition_Current);
            I2C_AcknowledgeConfig(I2C2, ENABLE);
            (void)I2C2->STAR2;
            i2c_set_buffer_interrupt(remaining > 3U);
            return;
    }
}

void i2c_handle_btf_from_isr(BaseType_t *higher_priority_task_woken) noexcept
{
    I2CPhase const phase = s_state.phase;

    switch(phase)
    {
        case I2CPhase::AddressRead:
            /*
             * The transmit BTF level can remain asserted briefly after START
             * is requested. The next meaningful event is SB.
             */
            return;

        case I2CPhase::Transmit:
        {
            /* If service was delayed until BTF while bytes remain, feeding
             * DATAR clears BTF and continues the transmit phase safely. */
            if(i2c_send_next_byte())
            {
                return;
            }

            if(s_state.read_len != 0U)
            {
                s_state.phase = I2CPhase::AddressRead;
                I2C_GenerateSTART(I2C2, ENABLE);
                return;
            }

            I2C_GenerateSTOP(I2C2, ENABLE);
            i2c_complete_from_isr(READY, higher_priority_task_woken);
            return;
        }

        case I2CPhase::Receive:
            break;

        default:
            i2c_fail_from_isr(higher_priority_task_woken);
            return;
    }

    size_t const remaining = i2c_read_remaining();
    switch(remaining)
    {
        case 0U:
        case 1U:
            i2c_fail_from_isr(higher_priority_task_woken);
            return;

        case 2U:
        {
            UBaseType_t const saved_mie = portSET_INTERRUPT_MASK_FROM_ISR();
            I2C_GenerateSTOP(I2C2, ENABLE);
            bool const first_ok = i2c_receive_next_byte();
            bool const second_ok = i2c_receive_next_byte();
            portCLEAR_INTERRUPT_MASK_FROM_ISR(saved_mie);

            i2c_complete_from_isr((first_ok && second_ok) ? READY : NoREADY,
                                  higher_priority_task_woken);
            return;
        }

        case 3U:
        {
            /* BTF holds SCL low with two bytes buffered. NACK the final byte,
             * consume the oldest byte, then wait for BTF with two remaining. */
            I2C_AcknowledgeConfig(I2C2, DISABLE);
            if(!i2c_receive_next_byte())
            {
                i2c_fail_from_isr(higher_priority_task_woken);
            }
            return;
        }

        default:
            if(!i2c_receive_next_byte())
            {
                i2c_fail_from_isr(higher_priority_task_woken);
                return;
            }
            if(i2c_read_remaining() == 3U)
            {
                i2c_set_buffer_interrupt(false);
            }
            return;
    }
}

void i2c_handle_rxne_from_isr(BaseType_t *higher_priority_task_woken) noexcept
{
    switch(s_state.phase)
    {
        case I2CPhase::Receive:
            break;

        default:
            i2c_fail_from_isr(higher_priority_task_woken);
            return;
    }

    size_t const remaining = i2c_read_remaining();
    switch(remaining)
    {
        case 0U:
            i2c_fail_from_isr(higher_priority_task_woken);
            return;

        case 1U:
        {
            bool const read_ok = i2c_receive_next_byte();
            i2c_complete_from_isr(read_ok ? READY : NoREADY,
                                  higher_priority_task_woken);
            return;
        }

        case 2U:
        case 3U:
            /* BTF owns these timing-sensitive final bytes. */
            i2c_set_buffer_interrupt(false);
            return;

        default:
            if(!i2c_receive_next_byte())
            {
                i2c_fail_from_isr(higher_priority_task_woken);
                return;
            }
            if(i2c_read_remaining() == 3U)
            {
                i2c_set_buffer_interrupt(false);
            }
            return;
    }
}

void i2c_event_from_isr(BaseType_t *higher_priority_task_woken) noexcept
{
    uint16_t const star1 = I2C2->STAR1;
    I2CPhase const phase = s_state.phase;

    if(i2c_clear_stop_flag(star1))
    {
        return;
    }

    if(!i2c_phase_is_active(phase))
    {
        i2c_disable_interrupts();
        return;
    }

    if((star1 & I2C_STAR1_SB) != 0U)
    {
        switch(phase)
        {
            case I2CPhase::AddressWrite:
                I2C_Send7bitAddress(I2C2,
                                    static_cast<uint8_t>(s_state.addr << 1U),
                                    I2C_Direction_Transmitter);
                break;

            case I2CPhase::AddressRead:
                I2C_Send7bitAddress(I2C2,
                                    static_cast<uint8_t>(s_state.addr << 1U),
                                    I2C_Direction_Receiver);
                break;

            default:
                i2c_fail_from_isr(higher_priority_task_woken);
                break;
        }
        return;
    }

    if((star1 & I2C_STAR1_ADDR) != 0U)
    {
        i2c_handle_address_from_isr(higher_priority_task_woken);
        return;
    }

    /* BTF takes precedence when it is present with TxE or RxNE. */
    if((star1 & I2C_STAR1_BTF) != 0U)
    {
        i2c_handle_btf_from_isr(higher_priority_task_woken);
        return;
    }

    if((star1 & I2C_STAR1_RXNE) != 0U)
    {
        i2c_handle_rxne_from_isr(higher_priority_task_woken);
        return;
    }

    if((star1 & I2C_STAR1_TXE) != 0U)
    {
        if((phase != I2CPhase::Transmit) || !i2c_send_next_byte())
        {
            i2c_set_buffer_interrupt(false);
        }
    }
}

void i2c_error_from_isr(BaseType_t *higher_priority_task_woken) noexcept
{
    uint16_t const errors = I2C2->STAR1 & I2C_ERROR_BITS;
    if(errors == 0U)
    {
        return;
    }

    /* Error flags are cleared by writing zero to each asserted bit. */
    I2C2->STAR1 = static_cast<uint16_t>(~errors);

    if(i2c_phase_is_active(s_state.phase))
    {
        I2C_GenerateSTOP(I2C2, ENABLE);
        i2c_complete_from_isr(NoREADY, higher_priority_task_woken);
    }
    else
    {
        i2c_disable_interrupts();
    }
}

ErrorStatus i2c_transaction(uint8_t addr_7bit,
                            const uint8_t *register_bytes,
                            size_t register_len,
                            const uint8_t *write_data,
                            size_t write_len,
                            uint8_t *read_data,
                            size_t read_len)
{
    if((addr_7bit > I2C_ADDRESS_MAX_7BIT) ||
       ((register_bytes == nullptr) && (register_len != 0U)) ||
       ((write_data == nullptr) && (write_len != 0U)) ||
       ((read_data == nullptr) && (read_len != 0U)) ||
       (register_len > I2C_WRITE_BUFFER_SIZE) ||
       (write_len > (I2C_WRITE_BUFFER_SIZE - register_len)))
    {
        return NoREADY;
    }

    std::lock_guard<FreeRTOSLock> const transaction_lock{s_lock};

    (void)ulTaskNotifyTakeIndexed(I2C_NOTIFICATION_INDEX, pdTRUE, 0U);

    if(!i2c_wait_bus_idle())
    {
        if(!i2c_recover_bus())
        {
            return NoREADY;
        }
    }

    uint8_t write_pos = 0U;
    for(size_t i = 0U; i < register_len; ++i)
    {
        s_state.write_buf[write_pos++] = register_bytes[i];
    }
    for(size_t i = 0U; i < write_len; ++i)
    {
        s_state.write_buf[write_pos++] = write_data[i];
    }

    s_state.waiting_task = xTaskGetCurrentTaskHandle();
    s_state.result = NoREADY;
    s_state.addr = addr_7bit;
    s_state.write_len = write_pos;
    s_state.write_pos = 0U;
    s_state.read_buf = read_data;
    s_state.read_len = read_len;
    s_state.read_pos = 0U;
    s_state.phase = I2CPhase::AddressWrite;

    (void)i2c_clear_stop_flag(I2C2->STAR1);
    I2C_NACKPositionConfig(I2C2, I2C_NACKPosition_Current);
    I2C_AcknowledgeConfig(I2C2, ENABLE);
    i2c_clear_error_flags();
    NVIC_ClearPendingIRQ(I2C2_EV_IRQn);
    NVIC_ClearPendingIRQ(I2C2_ER_IRQn);

    i2c_enable_interrupts();
    I2C_GenerateSTART(I2C2, ENABLE);

    (void)ulTaskNotifyTakeIndexed(I2C_NOTIFICATION_INDEX,
                                  pdTRUE,
                                  I2C_TRANSACTION_TIMEOUT_TICKS);

    bool completed;
    taskENTER_CRITICAL();
    completed = (s_state.phase == I2CPhase::Complete);
    if(!completed)
    {
        i2c_disable_interrupts();
        s_state.waiting_task = nullptr;
        s_state.phase = I2CPhase::Idle;
    }
    taskEXIT_CRITICAL();

    if(!completed)
    {
        (void)i2c_recover_bus();
        (void)ulTaskNotifyTakeIndexed(I2C_NOTIFICATION_INDEX, pdTRUE, 0U);
        return NoREADY;
    }

    ErrorStatus result = s_state.result;
    if(!i2c_wait_bus_idle())
    {
        result = NoREADY;
        (void)i2c_recover_bus();
    }
    else
    {
        I2C_NACKPositionConfig(I2C2, I2C_NACKPosition_Current);
        I2C_AcknowledgeConfig(I2C2, ENABLE);
    }

    taskENTER_CRITICAL();
    s_state.waiting_task = nullptr;
    s_state.phase = I2CPhase::Idle;
    taskEXIT_CRITICAL();

    /* Drain a completion that crossed the task timeout boundary after the
     * state was observed as Complete. */
    (void)ulTaskNotifyTakeIndexed(I2C_NOTIFICATION_INDEX, pdTRUE, 0U);
    return result;
}

} // namespace

extern "C" void i2c_hw_init(void)
{
    s_lock.init();

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_I2C2, ENABLE);

    i2c_configure_pins(GPIO_Mode_AF_OD);

    i2c_configure_peripheral();
    s_state.phase = I2CPhase::Idle;

    NVIC_ClearPendingIRQ(I2C2_EV_IRQn);
    NVIC_ClearPendingIRQ(I2C2_ER_IRQn);

    NVIC_InitTypeDef irq{};
    irq.NVIC_IRQChannelPreemptionPriority = 1U;
    irq.NVIC_IRQChannelCmd = ENABLE;

    irq.NVIC_IRQChannel = I2C2_ER_IRQn;
    irq.NVIC_IRQChannelSubPriority = 0U;
    NVIC_Init(&irq);

    irq.NVIC_IRQChannel = I2C2_EV_IRQn;
    irq.NVIC_IRQChannelSubPriority = 1U;
    NVIC_Init(&irq);
}

extern "C" ErrorStatus i2c_hw_scan_bus_at(uint8_t addr_7bit)
{
    return i2c_transaction(addr_7bit,
                           nullptr,
                           0U,
                           nullptr,
                           0U,
                           nullptr,
                           0U);
}

extern "C" ErrorStatus i2c_hw_write_register(uint8_t addr_7bit,
                                              uint8_t reg,
                                              uint8_t value)
{
    return i2c_transaction(addr_7bit,
                           &reg,
                           1U,
                           &value,
                           1U,
                           nullptr,
                           0U);
}

extern "C" ErrorStatus i2c_hw_write_register_burst(uint8_t addr_7bit,
                                                    uint8_t reg,
                                                    const uint8_t *data,
                                                    size_t len)
{
    return i2c_transaction(addr_7bit,
                           &reg,
                           1U,
                           data,
                           len,
                           nullptr,
                           0U);
}

extern "C" ErrorStatus i2c_hw_write_register16(uint8_t addr_7bit,
                                                uint16_t reg16,
                                                const uint8_t *data,
                                                size_t len)
{
    uint8_t const register_bytes[2] = {
        static_cast<uint8_t>((reg16 >> 8U) & 0xFFU),
        static_cast<uint8_t>(reg16 & 0xFFU),
    };
    return i2c_transaction(addr_7bit,
                           register_bytes,
                           sizeof(register_bytes),
                           data,
                           len,
                           nullptr,
                           0U);
}

extern "C" ErrorStatus i2c_hw_read_register16(uint8_t addr_7bit,
                                               uint16_t reg16,
                                               uint8_t *data,
                                               size_t len)
{
    uint8_t const register_bytes[2] = {
        static_cast<uint8_t>((reg16 >> 8U) & 0xFFU),
        static_cast<uint8_t>(reg16 & 0xFFU),
    };
    return i2c_transaction(addr_7bit,
                           register_bytes,
                           sizeof(register_bytes),
                           nullptr,
                           0U,
                           data,
                           len);
}

extern "C" ErrorStatus i2c_hw_read_register(uint8_t addr_7bit,
                                             uint8_t reg,
                                             uint8_t *value)
{
    return i2c_transaction(addr_7bit,
                           &reg,
                           1U,
                           nullptr,
                           0U,
                           value,
                           1U);
}

extern "C" {

PORT_ISR_BODY(I2C2_EV_IRQHandler)
{
    BaseType_t higher_priority_task_woken = pdFALSE;
    i2c_event_from_isr(&higher_priority_task_woken);
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

PORT_ISR_BODY(I2C2_ER_IRQHandler)
{
    BaseType_t higher_priority_task_woken = pdFALSE;
    i2c_error_from_isr(&higher_priority_task_woken);
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

} // extern "C"
