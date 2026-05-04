extern "C" {
#include "debug.h"
#include "hw/pinout.h"
#include "ch32v30x_dma.h"
#include "ch32v30x_gpio.h"
#include "ch32v30x_rcc.h"
#include "ch32v30x_sdio.h"
}

#include "sdio.h"

namespace sdcard {
namespace {

constexpr uint32_t CMD_TIMEOUT    = 0x00010000U;
constexpr uint32_t DATA_TIMEOUT   = 0x00100000U;
constexpr uint32_t FIFO_TIMEOUT   = 0x10000000U;
constexpr uint32_t DMA_TIMEOUT    = 0x10000000U;
constexpr uint32_t ICR_ALL        = 0x00C007FFU;
constexpr uint32_t ACMD41_RETRIES = 1000U;

constexpr uint32_t CMD8_ARG       = (1U << 8) | 0xAAU;
constexpr uint32_t CMD8_CHECK     = 0xAAU;
constexpr uint32_t ACMD41_HCS     = 1U << 30;
constexpr uint32_t ACMD41_VOLTAGE = 0x1FFU << 15;
constexpr uint32_t OCR_BUSY       = 1U << 31;
constexpr uint32_t OCR_CCS        = 1U << 30;

constexpr uint32_t ERR_FLAGS = SDIO_FLAG_RXOVERR | SDIO_FLAG_DCRCFAIL |
                               SDIO_FLAG_DTIMEOUT | SDIO_FLAG_STBITERR;

void clear_flags()
{
    SDIO->ICR = ICR_ALL;
}

void reset_data_path()
{
    SDIO->DCTRL = 0x0;

    SDIO_DataInitTypeDef data = {};
    data.SDIO_DataTimeOut   = DATA_TIMEOUT;
    data.SDIO_DataLength    = 0;
    data.SDIO_DataBlockSize = SDIO_DataBlockSize_1b;
    data.SDIO_TransferDir   = SDIO_TransferDir_ToCard;
    data.SDIO_TransferMode  = SDIO_TransferMode_Block;
    data.SDIO_DPSM          = SDIO_DPSM_Enable;
    SDIO_DataConfig(&data);

    clear_flags();
}

void stop_dma_read()
{
    SDIO_DMACmd(DISABLE);
    DMA_Cmd(DMA2_Channel4, DISABLE);
}

void configure_dma_read(std::span<uint8_t> buf)
{
    stop_dma_read();
    DMA_ClearFlag(DMA2_FLAG_GL4);

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

bool dma_buffer_aligned(std::span<uint8_t> buf)
{
    return (reinterpret_cast<uint32_t>(buf.data()) & 3U) == 0U &&
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
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_SDIO | RCC_AHBPeriph_DMA2, ENABLE);
    SDIO_DeInit();

    SDIO_InitTypeDef sdio = {};
    sdio.SDIO_ClockDiv            = 255U; // 144 MHz / (255+2) ~ 560 kHz
    sdio.SDIO_ClockEdge           = SDIO_ClockEdge_Rising;
    sdio.SDIO_ClockBypass         = SDIO_ClockBypass_Disable;
    sdio.SDIO_ClockPowerSave      = SDIO_ClockPowerSave_Disable;
    sdio.SDIO_BusWide             = SDIO_BusWide_1b;
    sdio.SDIO_HardwareFlowControl = SDIO_HardwareFlowControl_Disable;
    SDIO_Init(&sdio);

    SDIO_SetPowerState(SDIO_PowerState_ON);
    SDIO_ClockCmd(ENABLE);
    Delay_Ms(2);
}

void switch_fast()
{
    SDIO_ClockCmd(DISABLE);

    SDIO_InitTypeDef sdio = {};
    sdio.SDIO_ClockDiv            = 4U; // 144 MHz / (4+2) = 24 MHz
    sdio.SDIO_ClockEdge           = SDIO_ClockEdge_Rising;
    sdio.SDIO_ClockBypass         = SDIO_ClockBypass_Disable;
    sdio.SDIO_ClockPowerSave      = SDIO_ClockPowerSave_Disable;
    sdio.SDIO_BusWide             = SDIO_BusWide_4b;
    sdio.SDIO_HardwareFlowControl = SDIO_HardwareFlowControl_Disable;
    SDIO_Init(&sdio);

    SDIO_ClockCmd(ENABLE);
}

// --- command helpers (native SD mode) -------------------------------------

auto cmd_none(uint32_t idx, uint32_t arg) -> std::expected<void, ErrorStatus>
{
    clear_flags();

    SDIO_CmdInitTypeDef c = {};
    c.SDIO_Argument = arg;
    c.SDIO_CmdIndex = idx;
    c.SDIO_Response = SDIO_Response_No;
    c.SDIO_Wait     = SDIO_Wait_No;
    c.SDIO_CPSM     = SDIO_CPSM_Enable;
    SDIO_SendCommand(&c);

    for(uint32_t t = CMD_TIMEOUT; t; --t)
        if(SDIO->STA & SDIO_FLAG_CMDSENT)
        {
            clear_flags();
            return {};
        }

    clear_flags();
    return std::unexpected(NoREADY);
}

auto cmd_r1(uint32_t idx, uint32_t arg) -> std::expected<uint32_t, ErrorStatus>
{
    clear_flags();

    SDIO_CmdInitTypeDef c = {};
    c.SDIO_Argument = arg;
    c.SDIO_CmdIndex = idx;
    c.SDIO_Response = SDIO_Response_Short;
    c.SDIO_Wait     = SDIO_Wait_No;
    c.SDIO_CPSM     = SDIO_CPSM_Enable;
    SDIO_SendCommand(&c);

    for(uint32_t t = CMD_TIMEOUT; t; --t)
    {
        uint32_t sta = SDIO->STA;
        if(sta & SDIO_FLAG_CTIMEOUT) break;
        if(sta & SDIO_FLAG_CCRCFAIL) break;
        if(sta & SDIO_FLAG_CMDREND)
        {
            auto resp = SDIO_GetResponse(SDIO_RESP1);
            clear_flags();
            return resp;
        }
    }

    clear_flags();
    return std::unexpected(NoREADY);
}

// R3/R7: accepts CCRCFAIL as success (R3 has no CRC, R7 may differ)
auto cmd_r3(uint32_t idx, uint32_t arg) -> std::expected<uint32_t, ErrorStatus>
{
    clear_flags();

    SDIO_CmdInitTypeDef c = {};
    c.SDIO_Argument = arg;
    c.SDIO_CmdIndex = idx;
    c.SDIO_Response = SDIO_Response_Short;
    c.SDIO_Wait     = SDIO_Wait_No;
    c.SDIO_CPSM     = SDIO_CPSM_Enable;
    SDIO_SendCommand(&c);

    for(uint32_t t = CMD_TIMEOUT; t; --t)
    {
        uint32_t sta = SDIO->STA;
        if(sta & SDIO_FLAG_CTIMEOUT)
        {
            clear_flags();
            return std::unexpected(NoREADY);
        }
        if(sta & (SDIO_FLAG_CMDREND | SDIO_FLAG_CCRCFAIL))
        {
            auto resp = SDIO_GetResponse(SDIO_RESP1);
            clear_flags();
            return resp;
        }
    }

    clear_flags();
    return std::unexpected(NoREADY);
}

auto cmd_r2(uint32_t idx, uint32_t arg) -> std::expected<R2, ErrorStatus>
{
    clear_flags();

    SDIO_CmdInitTypeDef c = {};
    c.SDIO_Argument = arg;
    c.SDIO_CmdIndex = idx;
    c.SDIO_Response = SDIO_Response_Long;
    c.SDIO_Wait     = SDIO_Wait_No;
    c.SDIO_CPSM     = SDIO_CPSM_Enable;
    SDIO_SendCommand(&c);

    for(uint32_t t = CMD_TIMEOUT; t; --t)
    {
        uint32_t sta = SDIO->STA;
        if(sta & SDIO_FLAG_CTIMEOUT)
        {
            clear_flags();
            return std::unexpected(NoREADY);
        }
        if(sta & (SDIO_FLAG_CMDREND | SDIO_FLAG_CCRCFAIL))
        {
            R2 r = {{
                SDIO_GetResponse(SDIO_RESP1),
                SDIO_GetResponse(SDIO_RESP2),
                SDIO_GetResponse(SDIO_RESP3),
                SDIO_GetResponse(SDIO_RESP4),
            }};
            clear_flags();
            return r;
        }
    }

    clear_flags();
    return std::unexpected(NoREADY);
}

ErrorStatus read_fifo(std::span<uint8_t> buf, uint32_t end_flag)
{
    uint32_t count = 0U;
    uint32_t timeout = FIFO_TIMEOUT;

    auto store_word = [&](uint32_t word) {
        if(count + 4U <= buf.size())
        {
            buf[count]     = static_cast<uint8_t>(word);
            buf[count + 1] = static_cast<uint8_t>(word >> 8);
            buf[count + 2] = static_cast<uint8_t>(word >> 16);
            buf[count + 3] = static_cast<uint8_t>(word >> 24);
            count += 4U;
        }
    };

    while(!(SDIO->STA & (ERR_FLAGS | end_flag)) && timeout)
    {
        uint32_t sta = SDIO->STA;
        if(sta & SDIO_FLAG_RXFIFOHF)
        {
            for(int i = 0; i < 8; ++i)
                store_word(SDIO->FIFO);
            timeout = FIFO_TIMEOUT;
        }
        else if(sta & SDIO_FLAG_RXDAVL)
        {
            store_word(SDIO->FIFO);
            timeout = FIFO_TIMEOUT;
        }
        else
        {
            --timeout;
        }
    }

    while(SDIO->STA & SDIO_FLAG_RXDAVL)
        store_word(SDIO->FIFO);

    if(!timeout || (SDIO->STA & ERR_FLAGS))
    {
        clear_flags();
        return NoREADY;
    }

    if(count != buf.size())
    {
        clear_flags();
        return NoREADY;
    }

    clear_flags();
    return READY;
}

ErrorStatus wait_dma_read(uint32_t end_flag)
{
    uint32_t timeout = DMA_TIMEOUT;
    while(timeout)
    {
        if(DMA_GetFlagStatus(DMA2_FLAG_TE4) != RESET)
            break;

        uint32_t sta = SDIO->STA;
        if(sta & ERR_FLAGS)
            break;

        if(DMA_GetFlagStatus(DMA2_FLAG_TC4) != RESET)
        {
            while(!(SDIO->STA & (ERR_FLAGS | end_flag)) && timeout)
                --timeout;
            break;
        }

        --timeout;
    }

    bool ok = timeout &&
              DMA_GetFlagStatus(DMA2_FLAG_TE4) == RESET &&
              (SDIO->STA & ERR_FLAGS) == 0U &&
              (SDIO->STA & end_flag) &&
              DMA_GetCurrDataCounter(DMA2_Channel4) == 0U;

    stop_dma_read();
    DMA_ClearFlag(DMA2_FLAG_GL4);
    clear_flags();
    return ok ? READY : NoREADY;
}

} // anonymous namespace

// --- public interface -----------------------------------------------------

void SDIOTransport::init()
{
    init_gpio_1bit();
    reset_slow();
}

auto SDIOTransport::detect() -> std::expected<DetectResult, ErrorStatus>
{
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

    for(uint32_t i = 0; i < ACMD41_RETRIES && !ready; ++i)
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
            Delay_Ms(1);
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

    if(!sdhc)
        if(!cmd_r1(16, 512U))
            return std::unexpected(NoREADY);
    return DetectResult{parse_cid(*cid_r2), sdhc};
}

ErrorStatus SDIOTransport::read_blocks(uint32_t addr, std::span<uint8_t> buf)
{
    bool multi = buf.size() > 512U;
    bool use_dma = dma_buffer_aligned(buf);

    clear_flags();
    reset_data_path();
    SDIO->DCTRL = 0x0;

    SDIO_DataInitTypeDef data = {};
    data.SDIO_DataTimeOut   = DATA_TIMEOUT;
    data.SDIO_DataLength    = static_cast<uint32_t>(buf.size());
    data.SDIO_DataBlockSize = SDIO_DataBlockSize_512b;
    data.SDIO_TransferDir   = SDIO_TransferDir_ToSDIO;
    data.SDIO_TransferMode  = SDIO_TransferMode_Block;
    data.SDIO_DPSM          = SDIO_DPSM_Disable;
    SDIO_DataConfig(&data);

    if(use_dma)
        configure_dma_read(buf);

    auto r = cmd_r1(multi ? 18U : 17U, addr);
    if(!r)
    {
        stop_dma_read();
        return NoREADY;
    }

    if(use_dma)
    {
        SDIO_DMACmd(ENABLE);
        DMA_Cmd(DMA2_Channel4, ENABLE);
    }
    SDIO->DCTRL |= SDIO_DPSM_Enable;

    uint32_t end_flag = multi ? SDIO_FLAG_DATAEND : SDIO_FLAG_DBCKEND;
    ErrorStatus result = use_dma ? wait_dma_read(end_flag) : read_fifo(buf, end_flag);

    if(multi)
        cmd_r1(12, 0);

    return result;
}

} // namespace sdcard
