#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include "debug.h"
#include "types.h"

namespace sdcard {

class SDIOTransport {
public:
    static constexpr std::size_t max_blocks_per_transfer =
        UINT16_MAX / (512U / sizeof(uint32_t));

    void init();
    auto detect() -> std::expected<DetectResult, ErrorStatus>;
    ErrorStatus read_blocks(uint32_t addr, std::span<uint8_t> buf);
    ErrorStatus write_blocks(uint32_t addr, std::span<const uint8_t> buf);
    ErrorStatus sync();
    Status status() const;

private:
    ErrorStatus wait_ready();

    uint16_t rca_ = 0U;
};

} // namespace sdcard
