extern "C" {
#include "debug.h"
#include "hw/pinout.h"
#include "ch32v30x_gpio.h"
#include "ch32v30x_rcc.h"
}

#include "bitbang.h"

namespace sdcard {
namespace {

// SPI mode reuses the SDIO pins:
//   CLK  = PC12 (SDIO_CLK)
//   MOSI = PD2  (SDIO_CMD)
//   MISO = PC8  (SDIO_D0)
//   CS   = PC11 (SDIO_D3)

constexpr uint32_t R1_TIMEOUT         = 1000U;
constexpr uint32_t DATA_TOKEN_TIMEOUT = 100000U;
constexpr uint32_t ACMD41_RETRIES     = 1000U;

constexpr uint32_t CMD8_ARG       = (1U << 8) | 0xAAU;
constexpr uint32_t CMD8_CHECK     = 0xAAU;
constexpr uint32_t ACMD41_HCS     = 1U << 30;
constexpr uint32_t ACMD41_VOLTAGE = 0x1FFU << 15;

Status s_status = {.detected = false, .bus_width_bits = 1, .clock_hz = 400000U, .high_speed = false};

void cs_low()  { GPIO_WriteBit(SDIO_D3_GPIO_PORT, SDIO_D3_GPIO_PIN, Bit_RESET); }
void cs_high() { GPIO_WriteBit(SDIO_D3_GPIO_PORT, SDIO_D3_GPIO_PIN, Bit_SET); }

void clk_low()  { GPIO_WriteBit(SDIO_CLK_GPIO_PORT, SDIO_CLK_GPIO_PIN, Bit_RESET); }
void clk_high() { GPIO_WriteBit(SDIO_CLK_GPIO_PORT, SDIO_CLK_GPIO_PIN, Bit_SET); }

void mosi_write(uint8_t bit) { GPIO_WriteBit(SDIO_CMD_GPIO_PORT, SDIO_CMD_GPIO_PIN, bit ? Bit_SET : Bit_RESET); }
uint8_t miso_read() { return GPIO_ReadInputDataBit(SDIO_D0_GPIO_PORT, SDIO_D0_GPIO_PIN); }

uint8_t spi_xfer(uint8_t tx)
{
    uint8_t rx = 0;
    for(int i = 7; i >= 0; --i)
    {
        mosi_write((tx >> i) & 1U);
        Delay_Us(1);
        clk_high();
        Delay_Us(1);
        rx |= static_cast<uint8_t>(miso_read() << i);
        clk_low();
    }
    return rx;
}

uint8_t crc7(const uint8_t* data, uint32_t len)
{
    uint8_t crc = 0;
    for(uint32_t i = 0; i < len; ++i)
    {
        uint8_t byte = data[i];
        for(int bit = 7; bit >= 0; --bit)
        {
            crc = static_cast<uint8_t>((crc << 1) | ((byte >> bit) & 1U));
            if(crc & 0x80U)
                crc ^= 0x89U;
        }
    }
    for(int i = 0; i < 7; ++i)
    {
        crc = static_cast<uint8_t>(crc << 1);
        if(crc & 0x80U)
            crc ^= 0x89U;
    }
    return static_cast<uint8_t>((crc << 1) | 1U);
}

// --- SPI-mode command helpers ---------------------------------------------

void send_frame(uint32_t idx, uint32_t arg)
{
    uint8_t f[6];
    f[0] = static_cast<uint8_t>(0x40U | (idx & 0x3FU));
    f[1] = static_cast<uint8_t>(arg >> 24);
    f[2] = static_cast<uint8_t>(arg >> 16);
    f[3] = static_cast<uint8_t>(arg >> 8);
    f[4] = static_cast<uint8_t>(arg);
    f[5] = crc7(f, 5);
    for(auto b : f) spi_xfer(b);
}

std::expected<uint8_t, ErrorStatus> wait_r1()
{
    for(uint32_t i = 0; i < R1_TIMEOUT; ++i)
    {
        uint8_t r = spi_xfer(0xFF);
        if(!(r & 0x80U)) return r;
    }
    return std::unexpected(NoREADY);
}

// SPI R1 response (1 byte)
std::expected<uint8_t, ErrorStatus> spi_cmd(uint32_t idx, uint32_t arg)
{
    send_frame(idx, arg);
    return wait_r1();
}

struct R1_32 {
    uint8_t r1;
    uint32_t payload;
};

// SPI R1 + 32-bit response (CMD8/R7, CMD58/R3)
std::expected<R1_32, ErrorStatus> spi_cmd32(uint32_t idx, uint32_t arg)
{
    send_frame(idx, arg);
    auto r = wait_r1();
    if(!r) return std::unexpected(NoREADY);

    // R1 error bits [6:1] — reject if any set (bit 0 is idle-state, acceptable)
    if(*r & 0x7EU) return std::unexpected(NoREADY);

    uint32_t v = 0;
    v |= static_cast<uint32_t>(spi_xfer(0xFF)) << 24;
    v |= static_cast<uint32_t>(spi_xfer(0xFF)) << 16;
    v |= static_cast<uint32_t>(spi_xfer(0xFF)) << 8;
    v |= static_cast<uint32_t>(spi_xfer(0xFF));
    return R1_32{*r, v};
}

// Wait for 0xFE data token, then read N bytes + discard 2-byte CRC
ErrorStatus spi_read_data(std::span<uint8_t> buf)
{
    for(uint32_t i = 0; i < DATA_TOKEN_TIMEOUT; ++i)
    {
        uint8_t tok = spi_xfer(0xFF);
        if(tok == 0xFEU) goto got_token;
        if(tok != 0xFFU) return NoREADY;
    }
    return NoREADY;

got_token:
    for(auto& b : buf)
        b = spi_xfer(0xFF);
    spi_xfer(0xFF);
    spi_xfer(0xFF);
    return READY;
}

void init_gpio_slow()
{
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC | RCC_APB2Periph_GPIOD, ENABLE);

    GPIO_InitTypeDef gpio = {};
    gpio.GPIO_Speed = GPIO_Speed_50MHz;

    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio.GPIO_Pin = SDIO_CLK_GPIO_PIN | SDIO_D3_GPIO_PIN;
    GPIO_Init(GPIOC, &gpio);

    gpio.GPIO_Pin = SDIO_CMD_GPIO_PIN;
    GPIO_Init(GPIOD, &gpio);

    gpio.GPIO_Mode = GPIO_Mode_IPU;
    gpio.GPIO_Pin = SDIO_D0_GPIO_PIN | SDIO_D1_GPIO_PIN | SDIO_D2_GPIO_PIN;
    GPIO_Init(GPIOC, &gpio);

    cs_high();
    clk_low();
    mosi_write(1);

    for(int i = 0; i < 10; ++i)
        spi_xfer(0xFF);

    s_status = {.detected = false, .bus_width_bits = 1, .clock_hz = 400000U, .high_speed = false};
}

void switch_fast()
{
    GPIO_InitTypeDef gpio = {};
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;

    gpio.GPIO_Pin = SDIO_CLK_GPIO_PIN | SDIO_D3_GPIO_PIN;
    GPIO_Init(GPIOC, &gpio);

    gpio.GPIO_Pin = SDIO_CMD_GPIO_PIN;
    GPIO_Init(GPIOD, &gpio);

    s_status = {.detected = false, .bus_width_bits = 1, .clock_hz = 500000U, .high_speed = false};
}

} // anonymous namespace

// --- public interface -----------------------------------------------------

void BitbangTransport::init()
{
    init_gpio_slow();
}

auto BitbangTransport::detect() -> std::expected<DetectResult, ErrorStatus>
{
    init_gpio_slow();

    // CMD0: GO_IDLE_STATE — card enters SPI mode
    cs_low();
    auto r0 = spi_cmd(0, 0);
    cs_high(); spi_xfer(0xFF);
    if(!r0 || *r0 != 0x01U)
        return std::unexpected(NoREADY);

    // CMD8: SEND_IF_COND — detect SD v2+
    bool sd_v2 = false;
    cs_low();
    auto r8 = spi_cmd32(8, CMD8_ARG);
    cs_high(); spi_xfer(0xFF);
    if(r8 && (r8->payload & 0xFFU) == CMD8_CHECK)
        sd_v2 = true;

    // ACMD41: SD_SEND_OP_COND — wait until card is ready
    uint32_t acmd41_arg = ACMD41_VOLTAGE | (sd_v2 ? ACMD41_HCS : 0U);
    bool ready = false;

    for(uint32_t i = 0; i < ACMD41_RETRIES && !ready; ++i)
    {
        cs_low();
        auto r55 = spi_cmd(55, 0);
        cs_high(); spi_xfer(0xFF);
        if(!r55 || (*r55 & 0x7EU))
            return std::unexpected(NoREADY);

        cs_low();
        auto r41 = spi_cmd(41, acmd41_arg);
        cs_high(); spi_xfer(0xFF);

        if(r41 && *r41 == 0x00U)
            ready = true;
        else
            Delay_Ms(1);
    }

    if(!ready) return std::unexpected(NoREADY);

    // CMD58: READ_OCR — check CCS bit for SDHC
    bool sdhc = false;
    if(sd_v2)
    {
        cs_low();
        auto ocr = spi_cmd32(58, 0);
        cs_high(); spi_xfer(0xFF);
        if(ocr)
            sdhc = (ocr->payload & (1U << 30)) != 0;
    }

    // CMD10: SEND_CID — R1 + data block (16 bytes)
    cs_low();
    auto r10 = spi_cmd(10, 0);
    if(!r10 || (*r10 & 0x7EU))
    {
        cs_high(); spi_xfer(0xFF);
        return std::unexpected(NoREADY);
    }

    uint8_t cid_raw[16];
    if(spi_read_data(cid_raw) != READY)
    {
        cs_high(); spi_xfer(0xFF);
        return std::unexpected(NoREADY);
    }
    cs_high(); spi_xfer(0xFF);

    R2 r2 = {{
        (uint32_t(cid_raw[0])  << 24) | (uint32_t(cid_raw[1])  << 16) | (uint32_t(cid_raw[2])  << 8) | cid_raw[3],
        (uint32_t(cid_raw[4])  << 24) | (uint32_t(cid_raw[5])  << 16) | (uint32_t(cid_raw[6])  << 8) | cid_raw[7],
        (uint32_t(cid_raw[8])  << 24) | (uint32_t(cid_raw[9])  << 16) | (uint32_t(cid_raw[10]) << 8) | cid_raw[11],
        (uint32_t(cid_raw[12]) << 24) | (uint32_t(cid_raw[13]) << 16) | (uint32_t(cid_raw[14]) << 8) | cid_raw[15],
    }};

    // CMD16: SET_BLOCKLEN (SDSC only)
    if(!sdhc)
    {
        cs_low();
        auto r16 = spi_cmd(16, 512);
        cs_high(); spi_xfer(0xFF);
        if(!r16 || (*r16 & 0x7EU))
            return std::unexpected(NoREADY);
    }

    switch_fast();
    return DetectResult{parse_cid(r2), sdhc};
}

ErrorStatus BitbangTransport::read_blocks(uint32_t addr, std::span<uint8_t> buf)
{
    bool multi = buf.size() > 512U;

    cs_low();
    auto r = spi_cmd(multi ? 18U : 17U, addr);
    if(!r || (*r & 0x7EU))
    {
        cs_high(); spi_xfer(0xFF);
        return NoREADY;
    }

    uint32_t offset = 0;
    while(offset < buf.size())
    {
        if(spi_read_data(buf.subspan(offset, 512U)) != READY)
        {
            cs_high(); spi_xfer(0xFF);
            return NoREADY;
        }
        offset += 512U;
    }

    if(multi)
    {
        spi_cmd(12, 0);
        spi_xfer(0xFF);
    }

    cs_high();
    spi_xfer(0xFF);
    return READY;
}

Status BitbangTransport::status() const
{
    return s_status;
}

} // namespace sdcard
