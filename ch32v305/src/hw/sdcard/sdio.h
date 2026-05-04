#pragma once

#include <cstdint>
#include <expected>
#include <span>

#include "debug.h"
#include "types.h"

namespace sdcard {

class SDIOTransport {
public:
    void init();
    auto detect() -> std::expected<DetectResult, ErrorStatus>;
    ErrorStatus read_blocks(uint32_t addr, std::span<uint8_t> buf);
};

} // namespace sdcard
