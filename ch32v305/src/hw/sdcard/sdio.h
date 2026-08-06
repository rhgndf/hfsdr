#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include "debug.h"
#include "types.h"

namespace sdcard {

enum class SDIOWriteStage : uint32_t
{
    None,
    InvalidBuffer,
    BeforeCommand,
    Command,
    Data,
    StopCommand,
    AfterData,
};

struct SDIOWriteDiagnostics
{
    SDIOWriteStage stage = SDIOWriteStage::None;
    uintptr_t buffer = 0U;
    uint32_t length = 0U;
    uint32_t address = 0U;
    uint32_t command_events = 0U;
    uint32_t response = 0U;
    uint32_t data_events = 0U;
    uint32_t expected_data_events = 0U;
    uint32_t status = 0U;
    uint32_t data_count = 0U;
    uint32_t fifo_count = 0U;
    uint32_t data_control = 0U;
    uint32_t dma_remaining = 0U;
    uint32_t dma_config = 0U;
};

class SDIOTransport {
public:
    static constexpr std::size_t max_blocks_per_transfer =
        UINT16_MAX / (512U / sizeof(uint32_t));

    void init();
    auto detect(SwitchStatus switch_status)
        -> std::expected<DetectResult, ErrorStatus>;
    auto read_cid() -> std::expected<CID, ErrorStatus>;
    ErrorStatus read_blocks(uint32_t addr, std::span<uint8_t> buf);
    ErrorStatus write_blocks(uint32_t addr, std::span<const uint8_t> buf);
    ErrorStatus sync();
    Status status() const;

private:
    ErrorStatus wait_ready();

    uint16_t rca_ = 0U;
};

SDIOWriteDiagnostics const& last_write_diagnostics();

} // namespace sdcard
