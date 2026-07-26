extern "C" {
#include "FreeRTOS.h"
#include "task.h"

#include "freertos/port_isr.h"

#include "debug.h"
#include "hw/pinout.h"
#include "ch32v30x_dma.h"
#include "ch32v30x_gpio.h"
#include "ch32v30x_misc.h"
#include "ch32v30x_rcc.h"
#include "ch32v30x_sdio.h"
#include "system_ch32v30x.h"
}

#include "sdio.h"

#include <array>

namespace sdcard {
namespace {

constexpr uint32_t DATA_TIMEOUT = UINT32_MAX;
constexpr uint32_t ICR_ALL      = 0x00C007FFU;

constexpr UBaseType_t SDIO_NOTIFICATION_INDEX = 0U;
constexpr TickType_t COMMAND_TIMEOUT_TICKS = pdMS_TO_TICKS(100U);
constexpr TickType_t DATA_WAIT_TIMEOUT_TICKS = pdMS_TO_TICKS(1000U);
constexpr TickType_t CARD_READY_TIMEOUT_TICKS = pdMS_TO_TICKS(1000U);
constexpr TickType_t ACMD41_TIMEOUT_TICKS = pdMS_TO_TICKS(1000U);

constexpr uint32_t CMD8_ARG       = (1U << 8) | 0xAAU;
constexpr uint32_t CMD8_CHECK     = 0xAAU;
constexpr uint32_t CMD6_CHECK_HS  = 0x00FFFFF1U;
constexpr uint32_t CMD6_SWITCH_HS = 0x80FFFFF1U;
constexpr uint32_t ACMD41_HCS     = 1U << 30;
constexpr uint32_t ACMD41_VOLTAGE = 0x1FFU << 15;
constexpr uint32_t OCR_BUSY       = 1U << 31;
constexpr uint32_t OCR_CCS        = 1U << 30;
constexpr uint32_t R1_ERROR_MASK  = 0xFDFFE008U;
constexpr uint32_t R1_READY_FOR_DATA = 1U << 8;
constexpr uint32_t R1_CURRENT_STATE_MASK = 0xFU << 9;
constexpr uint32_t R1_CURRENT_STATE_TRANSFER = 4U << 9;

constexpr uint32_t DATA_ERROR_FLAGS = SDIO_FLAG_RXOVERR |
                                      SDIO_FLAG_TXUNDERR |
                                      SDIO_FLAG_DCRCFAIL |
                                      SDIO_FLAG_DTIMEOUT |
                                      SDIO_FLAG_STBITERR;
constexpr uint32_t COMMAND_FLAGS = SDIO_FLAG_CMDSENT |
                                   SDIO_FLAG_CMDREND |
                                   SDIO_FLAG_CCRCFAIL |
                                   SDIO_FLAG_CTIMEOUT;
constexpr uint32_t DATA_FLAGS = DATA_ERROR_FLAGS |
                                SDIO_FLAG_DATAEND |
                                SDIO_FLAG_DBCKEND;
constexpr uint32_t WAITABLE_SDIO_FLAGS = COMMAND_FLAGS | DATA_FLAGS;

constexpr uint32_t DMA_COMPLETE_EVENT = 1UL << 30;
constexpr uint32_t DMA_ERROR_EVENT    = 1UL << 31;

static_assert(configTASK_NOTIFICATION_ARRAY_ENTRIES > SDIO_NOTIFICATION_INDEX);
static_assert(COMMAND_TIMEOUT_TICKS > 0U);
static_assert(DATA_WAIT_TIMEOUT_TICKS > 0U);
static_assert(CARD_READY_TIMEOUT_TICKS > 0U);
static_assert(ACMD41_TIMEOUT_TICKS > 0U);

Status s_status = {.detected = false, .bus_width_bits = 1, .clock_hz = 400000U, .high_speed = false};
TaskHandle_t s_waiting_task = nullptr;
volatile bool s_start_write_data_on_command_complete = false;
volatile bool s_stop_write_on_data_end = false;
uint32_t s_last_command_events = 0U;
SDIOWriteDiagnostics s_last_write_diagnostics;

struct DMAWaitDiagnostics
{
    uint32_t events = 0U;
    uint32_t expected_events = 0U;
    uint32_t status = 0U;
    uint32_t data_count = 0U;
    uint32_t fifo_count = 0U;
    uint32_t data_control = 0U;
    uint32_t dma_remaining = 0U;
    uint32_t dma_config = 0U;
};

DMAWaitDiagnostics s_last_dma_wait_diagnostics;

void clear_flags(uint32_t flags)
{
    SDIO->ICR = flags & ICR_ALL;
}

void clear_all_flags()
{
    clear_flags(ICR_ALL);
}

void clear_task_notification(TaskHandle_t task)
{
    (void)xTaskNotifyStateClearIndexed(task, SDIO_NOTIFICATION_INDEX);
    (void)ulTaskNotifyValueClearIndexed(task,
                                        SDIO_NOTIFICATION_INDEX,
                                        UINT32_MAX);
}

uint32_t capture_dma_events()
{
    uint32_t events = 0U;
    if(DMA_GetFlagStatus(DMA2_FLAG_TC4) != RESET)
    {
        events |= DMA_COMPLETE_EVENT;
    }
    if(DMA_GetFlagStatus(DMA2_FLAG_TE4) != RESET)
    {
        events |= DMA_ERROR_EVENT;
    }
    if(events != 0U)
    {
        DMA_ClearFlag(DMA2_FLAG_GL4);
    }
    return events;
}

uint32_t capture_wait_events(uint32_t sdio_mask, bool include_dma)
{
    uint32_t events = SDIO->STA & sdio_mask;
    if(events != 0U)
    {
        clear_flags(events);
    }
    if(include_dma)
    {
        events |= capture_dma_events();
    }
    return events;
}

uint32_t arm_wait(uint32_t sdio_mask,
                  bool include_dma,
                  bool capture_existing)
{
    TaskHandle_t const current_task = xTaskGetCurrentTaskHandle();
    configASSERT(current_task != nullptr);
    configASSERT(s_waiting_task == nullptr);
    clear_task_notification(current_task);

    taskENTER_CRITICAL();
    s_waiting_task = current_task;

    uint32_t events = capture_existing
                          ? capture_wait_events(sdio_mask, include_dma)
                          : 0U;

    SDIO->MASK |= sdio_mask;
    if(include_dma)
    {
        DMA_ITConfig(DMA2_Channel4, DMA_IT_TC | DMA_IT_TE, ENABLE);
    }
    taskEXIT_CRITICAL();
    return events;
}

void disarm_wait(uint32_t sdio_mask, bool include_dma)
{
    TaskHandle_t const current_task = xTaskGetCurrentTaskHandle();
    uint32_t const cleanup_mask =
        include_dma ? DATA_FLAGS : sdio_mask;

    taskENTER_CRITICAL();
    SDIO->MASK &= ~cleanup_mask;
    if(include_dma)
    {
        DMA_ITConfig(DMA2_Channel4, DMA_IT_TC | DMA_IT_TE, DISABLE);
    }
    s_waiting_task = nullptr;
    clear_flags(cleanup_mask);
    if(include_dma)
    {
        DMA_ClearFlag(DMA2_FLAG_GL4);
        NVIC_ClearPendingIRQ(DMA2_Channel4_IRQn);
    }
    NVIC_ClearPendingIRQ(SDIO_IRQn);
    taskEXIT_CRITICAL();

    clear_task_notification(current_task);
}

bool events_complete(uint32_t events,
                     uint32_t success_mask,
                     uint32_t error_mask,
                     bool require_all_success)
{
    if((events & error_mask) != 0U)
    {
        return true;
    }
    if(require_all_success)
    {
        return (events & success_mask) == success_mask;
    }
    return (events & success_mask) != 0U;
}

uint32_t wait_for_events(uint32_t initial_events,
                         uint32_t success_mask,
                         uint32_t error_mask,
                         bool require_all_success,
                         TickType_t timeout_ticks,
                         uint32_t sdio_mask,
                         bool include_dma)
{
    uint32_t events = initial_events;
    TickType_t const start_tick = xTaskGetTickCount();

    while(!events_complete(events,
                           success_mask,
                           error_mask,
                           require_all_success))
    {
        TickType_t const elapsed_ticks = xTaskGetTickCount() - start_tick;
        if(elapsed_ticks >= timeout_ticks)
        {
            taskENTER_CRITICAL();
            events |= capture_wait_events(sdio_mask, include_dma);
            taskEXIT_CRITICAL();
            break;
        }

        uint32_t notification = 0U;
        BaseType_t const notified =
            xTaskNotifyWaitIndexed(SDIO_NOTIFICATION_INDEX,
                                   0U,
                                   UINT32_MAX,
                                   &notification,
                                   timeout_ticks - elapsed_ticks);
        events |= notification;

        if(notified != pdTRUE)
        {
            taskENTER_CRITICAL();
            events |= capture_wait_events(sdio_mask, include_dma);
            taskEXIT_CRITICAL();
            break;
        }
    }

    return events;
}

uint32_t clock_hz_from_div(uint32_t div)
{
    return SystemCoreClock / (div + 2U);
}

uint32_t div_for_max_hz(uint32_t max_hz)
{
    uint32_t div = (SystemCoreClock + max_hz - 1U) / max_hz - 2U;
    return div > 255U ? 255U : div;
}

void reset_data_path()
{
    s_start_write_data_on_command_complete = false;
    s_stop_write_on_data_end = false;
    SDIO->DCTRL = 0x0;
    clear_all_flags();
}

void stop_dma()
{
    DMA_ITConfig(DMA2_Channel4, DMA_IT_TC | DMA_IT_TE, DISABLE);
    SDIO_DMACmd(DISABLE);
    DMA_Cmd(DMA2_Channel4, DISABLE);
}

void configure_dma_read(std::span<uint8_t> buf)
{
    stop_dma();
    DMA_ClearFlag(DMA2_FLAG_GL4);
    NVIC_ClearPendingIRQ(DMA2_Channel4_IRQn);

    DMA_InitTypeDef dma = {};
    dma.DMA_PeripheralBaseAddr = reinterpret_cast<uint32_t>(&SDIO->FIFO);
    dma.DMA_MemoryBaseAddr     = reinterpret_cast<uint32_t>(buf.data());
    dma.DMA_DIR                = DMA_DIR_PeripheralSRC;
    dma.DMA_BufferSize         = static_cast<uint32_t>(buf.size()) / 4U;
    dma.DMA_PeripheralInc      = DMA_PeripheralInc_Disable;
    dma.DMA_MemoryInc          = DMA_MemoryInc_Enable;
    dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Word;
    dma.DMA_MemoryDataSize     = DMA_MemoryDataSize_Word;
    dma.DMA_Mode               = DMA_Mode_Normal;
    dma.DMA_Priority           = DMA_Priority_VeryHigh;
    dma.DMA_M2M                = DMA_M2M_Disable;
    DMA_Init(DMA2_Channel4, &dma);
}

void configure_dma_write(std::span<const uint8_t> buf)
{
    stop_dma();
    DMA_ClearFlag(DMA2_FLAG_GL4);
    NVIC_ClearPendingIRQ(DMA2_Channel4_IRQn);

    DMA_InitTypeDef dma = {};
    dma.DMA_PeripheralBaseAddr = reinterpret_cast<uint32_t>(&SDIO->FIFO);
    dma.DMA_MemoryBaseAddr     = reinterpret_cast<uint32_t>(buf.data());
    dma.DMA_DIR                = DMA_DIR_PeripheralDST;
    dma.DMA_BufferSize         = static_cast<uint32_t>(buf.size()) / 4U;
    dma.DMA_PeripheralInc      = DMA_PeripheralInc_Disable;
    dma.DMA_MemoryInc          = DMA_MemoryInc_Enable;
    dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Word;
    dma.DMA_MemoryDataSize     = DMA_MemoryDataSize_Word;
    dma.DMA_Mode               = DMA_Mode_Normal;
    dma.DMA_Priority           = DMA_Priority_VeryHigh;
    dma.DMA_M2M                = DMA_M2M_Disable;
    DMA_Init(DMA2_Channel4, &dma);
}

template<typename T>
bool dma_buffer_aligned(std::span<T> buf)
{
    return (reinterpret_cast<uintptr_t>(buf.data()) & 3U) == 0U &&
           (buf.size() & 3U) == 0U;
}

// 1-bit mode: CLK + CMD + D0 as AF_PP; D1-D3 as GPIO output high
void init_gpio_1bit()
{
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC | RCC_APB2Periph_GPIOD | RCC_APB2Periph_AFIO, ENABLE);

    GPIO_InitTypeDef gpio = {};
    gpio.GPIO_Speed = GPIO_Speed_50MHz;

    // CLK and D0 as AF
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    gpio.GPIO_Pin = SDIO_CLK_GPIO_PIN | SDIO_D0_GPIO_PIN;
    GPIO_Init(GPIOC, &gpio);

    // CMD as AF
    gpio.GPIO_Pin = SDIO_CMD_GPIO_PIN;
    GPIO_Init(GPIOD, &gpio);

    // D1-D3 as GPIO output high (D3 high prevents SPI mode entry on CMD0)
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio.GPIO_Pin = SDIO_D1_GPIO_PIN | SDIO_D2_GPIO_PIN | SDIO_D3_GPIO_PIN;
    GPIO_Init(GPIOC, &gpio);
    GPIO_WriteBit(SDIO_D1_GPIO_PORT, SDIO_D1_GPIO_PIN, Bit_SET);
    GPIO_WriteBit(SDIO_D2_GPIO_PORT, SDIO_D2_GPIO_PIN, Bit_SET);
    GPIO_WriteBit(SDIO_D3_GPIO_PORT, SDIO_D3_GPIO_PIN, Bit_SET);
}

// Switch D1-D3 to AF_PP for 4-bit SDIO
void switch_gpio_4bit()
{
    GPIO_InitTypeDef gpio = {};
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    gpio.GPIO_Pin = SDIO_D1_GPIO_PIN | SDIO_D2_GPIO_PIN | SDIO_D3_GPIO_PIN;
    GPIO_Init(GPIOC, &gpio);
}

void reset_slow()
{
    uint32_t clock_div = div_for_max_hz(400'000U);

    configASSERT(s_waiting_task == nullptr);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_SDIO | RCC_AHBPeriph_DMA2, ENABLE);
    stop_dma();
    SDIO->MASK = 0U;
    SDIO_DeInit();

    SDIO_InitTypeDef sdio = {};
    sdio.SDIO_ClockDiv            = clock_div;
    sdio.SDIO_ClockEdge           = SDIO_ClockEdge_Rising;
    sdio.SDIO_ClockBypass         = SDIO_ClockBypass_Disable;
    sdio.SDIO_ClockPowerSave      = SDIO_ClockPowerSave_Disable;
    sdio.SDIO_BusWide             = SDIO_BusWide_1b;
    sdio.SDIO_HardwareFlowControl = SDIO_HardwareFlowControl_Disable;
    SDIO_Init(&sdio);

    SDIO_SetPowerState(SDIO_PowerState_ON);
    SDIO_ClockCmd(ENABLE);
    s_status = {.detected = false,
                .bus_width_bits = 1,
                .clock_hz = clock_hz_from_div(clock_div),
                .high_speed = false};
    vTaskDelay(pdMS_TO_TICKS(2U));
}

void init_interrupts()
{
    SDIO->MASK = 0U;
    DMA_ITConfig(DMA2_Channel4, DMA_IT_TC | DMA_IT_TE, DISABLE);
    clear_all_flags();
    DMA_ClearFlag(DMA2_FLAG_GL4);
    NVIC_ClearPendingIRQ(SDIO_IRQn);
    NVIC_ClearPendingIRQ(DMA2_Channel4_IRQn);

    NVIC_InitTypeDef irq = {};
    irq.NVIC_IRQChannelPreemptionPriority = 2U;
    irq.NVIC_IRQChannelCmd = ENABLE;

    irq.NVIC_IRQChannel = SDIO_IRQn;
    irq.NVIC_IRQChannelSubPriority = 0U;
    NVIC_Init(&irq);

    irq.NVIC_IRQChannel = DMA2_Channel4_IRQn;
    irq.NVIC_IRQChannelSubPriority = 1U;
    NVIC_Init(&irq);
}

void switch_fast()
{
    uint32_t clock_div = div_for_max_hz(25'000'000U);

    SDIO_ClockCmd(DISABLE);

    SDIO_InitTypeDef sdio = {};
    sdio.SDIO_ClockDiv            = clock_div;
    sdio.SDIO_ClockEdge           = SDIO_ClockEdge_Rising;
    sdio.SDIO_ClockBypass         = SDIO_ClockBypass_Disable;
    sdio.SDIO_ClockPowerSave      = SDIO_ClockPowerSave_Disable;
    sdio.SDIO_BusWide             = SDIO_BusWide_4b;
    sdio.SDIO_HardwareFlowControl = SDIO_HardwareFlowControl_Disable;
    SDIO_Init(&sdio);

    SDIO_ClockCmd(ENABLE);
    s_status = {.detected = false,
                .bus_width_bits = 4,
                .clock_hz = clock_hz_from_div(clock_div),
                .high_speed = false};
}

void switch_high_speed_clock()
{
    // UHS-I can go up to 50MHz, but this board only seems to work up to 36MHz
    uint32_t clock_div = div_for_max_hz(36'000'000U);

    SDIO_ClockCmd(DISABLE);

    SDIO_InitTypeDef sdio = {};
    sdio.SDIO_ClockDiv            = clock_div;
    sdio.SDIO_ClockEdge           = SDIO_ClockEdge_Rising;
    sdio.SDIO_ClockBypass         = SDIO_ClockBypass_Disable;
    sdio.SDIO_ClockPowerSave      = SDIO_ClockPowerSave_Disable;
    sdio.SDIO_BusWide             = SDIO_BusWide_4b;
    sdio.SDIO_HardwareFlowControl = SDIO_HardwareFlowControl_Disable;
    SDIO_Init(&sdio);

    SDIO_ClockCmd(ENABLE);
    s_status = {.detected = false,
                .bus_width_bits = 4,
                .clock_hz = clock_hz_from_div(clock_div),
                .high_speed = true};
}

// --- command helpers (native SD mode) -------------------------------------

uint32_t execute_command(uint32_t idx,
                         uint32_t arg,
                         uint32_t response,
                         uint32_t success_flags,
                         uint32_t error_flags)
{
    uint32_t const irq_mask = success_flags | error_flags;

    SDIO->MASK &= ~COMMAND_FLAGS;
    clear_flags(COMMAND_FLAGS);
    NVIC_ClearPendingIRQ(SDIO_IRQn);
    (void)arm_wait(irq_mask, false, false);

    SDIO_CmdInitTypeDef c = {};
    c.SDIO_Argument = arg;
    c.SDIO_CmdIndex = idx;
    c.SDIO_Response = response;
    c.SDIO_Wait     = SDIO_Wait_No;
    c.SDIO_CPSM     = SDIO_CPSM_Enable;
    SDIO_SendCommand(&c);

    uint32_t const events =
        wait_for_events(0U,
                        success_flags,
                        error_flags,
                        false,
                        COMMAND_TIMEOUT_TICKS,
                        irq_mask,
                        false);
    disarm_wait(irq_mask, false);
    s_last_command_events = events;
    return events;
}

void send_stop_command()
{
    /*
     * CMD12 must follow DATAEND promptly. Leave its response flags unmasked:
     * they remain latched until the task arms its command wait and collects
     * either the already-complete response or the subsequent interrupt.
     */
    SDIO->MASK &= ~COMMAND_FLAGS;
    clear_flags(COMMAND_FLAGS);

    SDIO_CmdInitTypeDef command = {};
    command.SDIO_Argument = 0U;
    command.SDIO_CmdIndex = 12U;
    command.SDIO_Response = SDIO_Response_Short;
    command.SDIO_Wait     = SDIO_Wait_No;
    command.SDIO_CPSM     = SDIO_CPSM_Enable;
    SDIO_SendCommand(&command);
}

void start_pending_stop_from_task()
{
    taskENTER_CRITICAL();
    if(s_stop_write_on_data_end)
    {
        s_stop_write_on_data_end = false;
        send_stop_command();
    }
    taskEXIT_CRITICAL();
}

auto wait_started_r1(uint32_t idx) -> std::expected<uint32_t, ErrorStatus>
{
    constexpr uint32_t success_flags = SDIO_FLAG_CMDREND;
    constexpr uint32_t error_flags =
        SDIO_FLAG_CTIMEOUT | SDIO_FLAG_CCRCFAIL;
    constexpr uint32_t irq_mask = success_flags | error_flags;

    uint32_t const initial_events = arm_wait(irq_mask, false, true);
    uint32_t const events =
        wait_for_events(initial_events,
                        success_flags,
                        error_flags,
                        false,
                        COMMAND_TIMEOUT_TICKS,
                        irq_mask,
                        false);
    disarm_wait(irq_mask, false);
    s_last_command_events = events;

    if((events & success_flags) != 0U &&
       (events & error_flags) == 0U &&
       SDIO_GetCommandResponse() == idx)
    {
        return SDIO_GetResponse(SDIO_RESP1);
    }
    return std::unexpected(NoREADY);
}

auto cmd_none(uint32_t idx, uint32_t arg) -> std::expected<void, ErrorStatus>
{
    uint32_t const events =
        execute_command(idx,
                        arg,
                        SDIO_Response_No,
                        SDIO_FLAG_CMDSENT,
                        0U);
    if((events & SDIO_FLAG_CMDSENT) != 0U)
    {
        return {};
    }
    return std::unexpected(NoREADY);
}

auto cmd_r1(uint32_t idx, uint32_t arg) -> std::expected<uint32_t, ErrorStatus>
{
    uint32_t const events =
        execute_command(idx,
                        arg,
                        SDIO_Response_Short,
                        SDIO_FLAG_CMDREND,
                        SDIO_FLAG_CTIMEOUT | SDIO_FLAG_CCRCFAIL);
    if((events & SDIO_FLAG_CMDREND) != 0U &&
       (events & (SDIO_FLAG_CTIMEOUT | SDIO_FLAG_CCRCFAIL)) == 0U)
    {
        return SDIO_GetResponse(SDIO_RESP1);
    }
    return std::unexpected(NoREADY);
}

// R3/R7: accepts CCRCFAIL as success (R3 has no CRC, R7 may differ)
auto cmd_r3(uint32_t idx, uint32_t arg) -> std::expected<uint32_t, ErrorStatus>
{
    uint32_t const success_flags =
        SDIO_FLAG_CMDREND | SDIO_FLAG_CCRCFAIL;
    uint32_t const events =
        execute_command(idx,
                        arg,
                        SDIO_Response_Short,
                        success_flags,
                        SDIO_FLAG_CTIMEOUT);
    if((events & success_flags) != 0U &&
       (events & SDIO_FLAG_CTIMEOUT) == 0U)
    {
        return SDIO_GetResponse(SDIO_RESP1);
    }
    return std::unexpected(NoREADY);
}

auto cmd_r2(uint32_t idx, uint32_t arg) -> std::expected<R2, ErrorStatus>
{
    uint32_t const success_flags =
        SDIO_FLAG_CMDREND | SDIO_FLAG_CCRCFAIL;
    uint32_t const events =
        execute_command(idx,
                        arg,
                        SDIO_Response_Long,
                        success_flags,
                        SDIO_FLAG_CTIMEOUT);
    if((events & success_flags) != 0U &&
       (events & SDIO_FLAG_CTIMEOUT) == 0U)
    {
        return R2{{
            SDIO_GetResponse(SDIO_RESP1),
            SDIO_GetResponse(SDIO_RESP2),
            SDIO_GetResponse(SDIO_RESP3),
            SDIO_GetResponse(SDIO_RESP4),
        }};
    }
    return std::unexpected(NoREADY);
}

struct DMAWait
{
    uint32_t end_flag;
    uint32_t irq_mask;
    uint32_t initial_events;
};

DMAWait arm_dma_wait(uint32_t end_flag)
{
    uint32_t const irq_mask = DATA_ERROR_FLAGS | end_flag;
    return {
        .end_flag = end_flag,
        .irq_mask = irq_mask,
        .initial_events = arm_wait(irq_mask, true, true),
    };
}

ErrorStatus complete_dma_wait(DMAWait const& wait)
{
    uint32_t const success_mask = DMA_COMPLETE_EVENT | wait.end_flag;
    uint32_t const error_mask = DMA_ERROR_EVENT | DATA_ERROR_FLAGS;
    uint32_t const events =
        wait_for_events(wait.initial_events,
                        success_mask,
                        error_mask,
                        true,
                        DATA_WAIT_TIMEOUT_TICKS,
                        wait.irq_mask,
                        true);

    bool const ok = (events & error_mask) == 0U &&
                    (events & success_mask) == success_mask &&
                    DMA_GetCurrDataCounter(DMA2_Channel4) == 0U;

    s_last_dma_wait_diagnostics = {
        .events = events,
        .expected_events = success_mask,
        .status = SDIO->STA,
        .data_count = SDIO->DCOUNT,
        .fifo_count = SDIO->FIFOCNT,
        .data_control = SDIO->DCTRL,
        .dma_remaining = DMA_GetCurrDataCounter(DMA2_Channel4),
        .dma_config = DMA2_Channel4->CFGR,
    };

    disarm_wait(wait.irq_mask, true);
    stop_dma();
    return ok ? READY : NoREADY;
}

void record_write_stage(SDIOWriteStage stage, uint32_t response = 0U)
{
    s_last_write_diagnostics.stage = stage;
    s_last_write_diagnostics.command_events = s_last_command_events;
    s_last_write_diagnostics.response = response;
}

void record_write_data_diagnostics()
{
    s_last_write_diagnostics.data_events =
        s_last_dma_wait_diagnostics.events;
    s_last_write_diagnostics.expected_data_events =
        s_last_dma_wait_diagnostics.expected_events;
    s_last_write_diagnostics.status =
        s_last_dma_wait_diagnostics.status;
    s_last_write_diagnostics.data_count =
        s_last_dma_wait_diagnostics.data_count;
    s_last_write_diagnostics.fifo_count =
        s_last_dma_wait_diagnostics.fifo_count;
    s_last_write_diagnostics.data_control =
        s_last_dma_wait_diagnostics.data_control;
    s_last_write_diagnostics.dma_remaining =
        s_last_dma_wait_diagnostics.dma_remaining;
    s_last_write_diagnostics.dma_config =
        s_last_dma_wait_diagnostics.dma_config;
}

ErrorStatus read_switch_status(uint32_t arg, std::span<uint8_t, 64> status)
{
    reset_data_path();
    configure_dma_read(status);

    SDIO_DataInitTypeDef data = {};
    data.SDIO_DataTimeOut   = DATA_TIMEOUT;
    data.SDIO_DataLength    = static_cast<uint32_t>(status.size());
    data.SDIO_DataBlockSize = SDIO_DataBlockSize_64b;
    data.SDIO_TransferDir   = SDIO_TransferDir_ToSDIO;
    data.SDIO_TransferMode  = SDIO_TransferMode_Block;
    data.SDIO_DPSM          = SDIO_DPSM_Enable;
    SDIO_DataConfig(&data);

    /*
     * Configure the new data path before enabling RX DMA. Enabling DMA first
     * can consume a word left in the FIFO by the preceding CMD6 transfer.
     */
    SDIO_DMACmd(ENABLE);
    DMA_Cmd(DMA2_Channel4, ENABLE);

    auto r = cmd_r1(6, arg);
    if(!r)
    {
        stop_dma();
        reset_data_path();
        return NoREADY;
    }

    DMAWait const wait = arm_dma_wait(SDIO_FLAG_DBCKEND);
    ErrorStatus const result = complete_dma_wait(wait);
    if(result != READY)
    {
        reset_data_path();
    }
    return result;
}

bool switch_card_high_speed()
{
    alignas(4) std::array<uint8_t, 64> status = {};

    if(read_switch_status(CMD6_CHECK_HS, status) != READY)
        return false;

    uint16_t group1_supported =
        (static_cast<uint16_t>(status[12]) << 8) | status[13];
    if((group1_supported & (1U << 1)) == 0U)
        return false;

    status = {};
    if(read_switch_status(CMD6_SWITCH_HS, status) != READY)
        return false;

    uint8_t selected = status[16] & 0x0FU;
    if(selected != 1U)
        return false;

    return true;
}

} // anonymous namespace

// --- public interface -----------------------------------------------------

void SDIOTransport::init()
{
    init_gpio_1bit();
    reset_slow();
    init_interrupts();
}

auto SDIOTransport::detect() -> std::expected<DetectResult, ErrorStatus>
{
    rca_ = 0U;
    init_gpio_1bit();
    reset_slow();

    bool cmd0_ok = false;
    for(int i = 0; i < 74; ++i)
        if(cmd_none(0, 0)) { cmd0_ok = true; break; }
    if(!cmd0_ok)
        return std::unexpected(NoREADY);

    bool sd_v2 = false;
    if(auto r = cmd_r3(8, CMD8_ARG))
        if((*r & 0xFFU) == CMD8_CHECK)
            sd_v2 = true;

    uint32_t acmd41_arg = ACMD41_VOLTAGE | (sd_v2 ? ACMD41_HCS : 0U);
    bool sdhc = false;
    bool ready = false;

    TickType_t const acmd41_start_tick = xTaskGetTickCount();
    while(!ready &&
          (xTaskGetTickCount() - acmd41_start_tick) <
              ACMD41_TIMEOUT_TICKS)
    {
        if(!cmd_r1(55, 0))
            return std::unexpected(NoREADY);

        auto r41 = cmd_r3(41, acmd41_arg);
        if(!r41) return std::unexpected(NoREADY);

        if(*r41 & OCR_BUSY)
        {
            sdhc = sd_v2 && (*r41 & OCR_CCS);
            ready = true;
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(1U));
        }
    }

    if(!ready) return std::unexpected(NoREADY);
    auto cid_r2 = cmd_r2(2, 0);
    if(!cid_r2) return std::unexpected(NoREADY);

    auto rca_resp = cmd_r1(3, 0);
    if(!rca_resp) return std::unexpected(NoREADY);
    uint16_t rca = static_cast<uint16_t>(*rca_resp >> 16);

    if(!cmd_r1(7, static_cast<uint32_t>(rca) << 16))
        return std::unexpected(NoREADY);

    if(!cmd_r1(55, static_cast<uint32_t>(rca) << 16))
        return std::unexpected(NoREADY);
    auto acmd6 = cmd_r1(6, 2U);
    if(!acmd6)
        return std::unexpected(NoREADY);

    switch_gpio_4bit();
    switch_fast();
    if(switch_card_high_speed())
        switch_high_speed_clock();

    if(!sdhc)
        if(!cmd_r1(16, 512U))
            return std::unexpected(NoREADY);

    rca_ = rca;
    return DetectResult{parse_cid(*cid_r2), sdhc};
}

// DMA is required at high clock speeds; all callers provide an aligned buffer.
ErrorStatus SDIOTransport::read_blocks(uint32_t addr, std::span<uint8_t> buf)
{
    if(buf.empty() || buf.size() % 512U != 0U ||
       buf.size() > max_blocks_per_transfer * 512U ||
       !dma_buffer_aligned(buf))
    {
        return NoREADY;
    }
    bool const multi = buf.size() > 512U;

    reset_data_path();
    configure_dma_read(buf);

    SDIO_DataInitTypeDef data = {};
    data.SDIO_DataTimeOut   = DATA_TIMEOUT;
    data.SDIO_DataLength    = static_cast<uint32_t>(buf.size());
    data.SDIO_DataBlockSize = SDIO_DataBlockSize_512b;
    data.SDIO_TransferDir   = SDIO_TransferDir_ToSDIO;
    data.SDIO_TransferMode  = SDIO_TransferMode_Block;
    data.SDIO_DPSM          = SDIO_DPSM_Enable;
    SDIO_DataConfig(&data);

    /*
     * Start DMA only after DataConfig has established a clean RX FIFO, but
     * before CMD17/CMD18 so task scheduling cannot delay servicing the card.
     */
    SDIO_DMACmd(ENABLE);
    DMA_Cmd(DMA2_Channel4, ENABLE);

    auto r = cmd_r1(multi ? 18U : 17U, addr);
    if(!r)
    {
        stop_dma();
        reset_data_path();
        return NoREADY;
    }

    uint32_t const end_flag =
        multi ? SDIO_FLAG_DATAEND : SDIO_FLAG_DBCKEND;
    DMAWait const wait = arm_dma_wait(end_flag);
    ErrorStatus result = complete_dma_wait(wait);

    if(multi)
    {
        auto const stop_response = cmd_r1(12U, 0U);
        if(!stop_response ||
           (*stop_response & R1_ERROR_MASK) != 0U)
        {
            result = NoREADY;
        }
    }

    if(result != READY)
    {
        reset_data_path();
    }

    return result;
}

ErrorStatus SDIOTransport::wait_ready()
{
    if(rca_ == 0U)
    {
        return NoREADY;
    }

    TickType_t const start_tick = xTaskGetTickCount();
    while((xTaskGetTickCount() - start_tick) <
          CARD_READY_TIMEOUT_TICKS)
    {
        auto response = cmd_r1(13U, static_cast<uint32_t>(rca_) << 16);
        if(!response || (*response & R1_ERROR_MASK) != 0U)
        {
            return NoREADY;
        }

        bool ready = (*response & R1_READY_FOR_DATA) != 0U;
        bool transfer_state =
            (*response & R1_CURRENT_STATE_MASK) == R1_CURRENT_STATE_TRANSFER;
        if(ready && transfer_state)
        {
            return READY;
        }

        vTaskDelay(pdMS_TO_TICKS(1U));
    }

    return NoREADY;
}

ErrorStatus SDIOTransport::write_blocks(uint32_t addr,
                                        std::span<const uint8_t> buf)
{
    s_last_write_diagnostics = {
        .buffer = reinterpret_cast<uintptr_t>(buf.data()),
        .length = static_cast<uint32_t>(buf.size()),
        .address = addr,
    };

    if(buf.empty() || buf.size() % 512U != 0U ||
       buf.size() > max_blocks_per_transfer * 512U ||
       !dma_buffer_aligned(buf))
    {
        record_write_stage(SDIOWriteStage::InvalidBuffer);
        return NoREADY;
    }
    if(wait_ready() != READY)
    {
        record_write_stage(SDIOWriteStage::BeforeCommand);
        return NoREADY;
    }

    bool const multi = buf.size() > 512U;
    if(multi)
    {
        auto const app_command =
            cmd_r1(55U, static_cast<uint32_t>(rca_) << 16);
        if(!app_command ||
           (*app_command & R1_ERROR_MASK) != 0U)
        {
            record_write_stage(SDIOWriteStage::Command,
                               app_command.value_or(0U));
            return NoREADY;
        }

        auto const preerase =
            cmd_r1(23U,
                   static_cast<uint32_t>(buf.size() / 512U));
        if(!preerase || (*preerase & R1_ERROR_MASK) != 0U)
        {
            record_write_stage(SDIOWriteStage::Command,
                               preerase.value_or(0U));
            return NoREADY;
        }
    }

    reset_data_path();
    configure_dma_write(buf);

    /*
     * Set up the data registers and DMA channel before CMD24/CMD25. The
     * recording task is lower priority than the I2S task, so it is not
     * guaranteed to run soon enough after the command-response interrupt to
     * start the data phase itself. CMDREND starts DTEN and then DMAEN in the
     * SDIO ISR instead.
     */
    SDIO_DataInitTypeDef data = {};
    data.SDIO_DataTimeOut   = DATA_TIMEOUT;
    data.SDIO_DataLength    = static_cast<uint32_t>(buf.size());
    data.SDIO_DataBlockSize = SDIO_DataBlockSize_512b;
    data.SDIO_TransferDir   = SDIO_TransferDir_ToCard;
    data.SDIO_TransferMode  = SDIO_TransferMode_Block;
    data.SDIO_DPSM          = SDIO_DPSM_Disable;
    SDIO_DataConfig(&data);

    /*
     * Arm the channel, but leave SDIO DMA requests disabled. Enabling DMAEN
     * here lets DMA preload the FIFO before DTEN loads the data counter,
     * which can leave DCOUNT offset by the preloaded words.
     */
    DMA_Cmd(DMA2_Channel4, ENABLE);
    s_start_write_data_on_command_complete = true;
    s_stop_write_on_data_end = multi;

    auto response = cmd_r1(multi ? 25U : 24U, addr);
    s_start_write_data_on_command_complete = false;
    if(!response || (*response & R1_ERROR_MASK) != 0U)
    {
        s_stop_write_on_data_end = false;
        record_write_stage(SDIOWriteStage::Command,
                           response.value_or(0U));
        stop_dma();
        return NoREADY;
    }

    uint32_t const end_flag =
        multi ? SDIO_FLAG_DATAEND : SDIO_FLAG_DBCKEND;
    DMAWait const wait = arm_dma_wait(end_flag);
    if((wait.initial_events & SDIO_FLAG_DATAEND) != 0U)
    {
        start_pending_stop_from_task();
    }

    ErrorStatus result = complete_dma_wait(wait);
    record_write_data_diagnostics();

    if(multi)
    {
        // Also abort an errored transfer that never reached DATAEND.
        start_pending_stop_from_task();
        auto const stop_response = wait_started_r1(12U);
        if(!stop_response || (*stop_response & R1_ERROR_MASK) != 0U)
        {
            record_write_stage(SDIOWriteStage::StopCommand,
                               stop_response.value_or(0U));
            result = NoREADY;
        }
    }

    if(result != READY)
    {
        if(s_last_write_diagnostics.stage !=
           SDIOWriteStage::StopCommand)
        {
            record_write_stage(SDIOWriteStage::Data,
                               *response);
        }
        reset_data_path();
        return NoREADY;
    }

    ErrorStatus const ready = wait_ready();
    if(ready != READY)
    {
        record_write_stage(SDIOWriteStage::AfterData,
                           *response);
    }
    return ready;
}

ErrorStatus SDIOTransport::sync()
{
    return wait_ready();
}

Status SDIOTransport::status() const
{
    return s_status;
}

SDIOWriteDiagnostics const& last_write_diagnostics()
{
    return s_last_write_diagnostics;
}

extern "C" {

PORT_ISR_BODY(SDIO_IRQHandler)
{
    BaseType_t higher_priority_task_woken = pdFALSE;
    uint32_t const events =
        SDIO->STA & SDIO->MASK & WAITABLE_SDIO_FLAGS;
    if(events != 0U)
    {
        if(s_start_write_data_on_command_complete &&
           (events & SDIO_FLAG_CMDREND) != 0U)
        {
            s_start_write_data_on_command_complete = false;
            SDIO->DCTRL |= SDIO_DPSM_Enable;
            SDIO_DMACmd(ENABLE);
        }

        if(s_stop_write_on_data_end &&
           (events & SDIO_FLAG_DATAEND) != 0U)
        {
            s_stop_write_on_data_end = false;
            send_stop_command();
        }

        clear_flags(events);
        TaskHandle_t const waiting_task = s_waiting_task;
        if(waiting_task != nullptr)
        {
            (void)xTaskNotifyIndexedFromISR(waiting_task,
                                            SDIO_NOTIFICATION_INDEX,
                                            events,
                                            eSetBits,
                                            &higher_priority_task_woken);
        }
    }
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

PORT_ISR_BODY(DMA2_Channel4_IRQHandler)
{
    BaseType_t higher_priority_task_woken = pdFALSE;
    uint32_t events = 0U;
    if(DMA_GetITStatus(DMA2_IT_TC4) != RESET)
    {
        events |= DMA_COMPLETE_EVENT;
    }
    if(DMA_GetITStatus(DMA2_IT_TE4) != RESET)
    {
        events |= DMA_ERROR_EVENT;
    }

    DMA_ClearITPendingBit(DMA2_IT_GL4);
    TaskHandle_t const waiting_task = s_waiting_task;
    if(events != 0U && waiting_task != nullptr)
    {
        (void)xTaskNotifyIndexedFromISR(waiting_task,
                                        SDIO_NOTIFICATION_INDEX,
                                        events,
                                        eSetBits,
                                        &higher_priority_task_woken);
    }
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

} // extern "C"

} // namespace sdcard
