#pragma once

#include <cstdint>
#include <expected>
#include <span>

#include "debug.h"
#include "types.h"

namespace sdcard {

class BitbangTransport {
public:
    void init();
    auto detect(SwitchStatus switch_status)
        -> std::expected<DetectResult, ErrorStatus>;
    auto read_cid() -> std::expected<CID, ErrorStatus>;
    ErrorStatus read_blocks(uint32_t addr, std::span<uint8_t> buf);
    Status status() const;
};

} // namespace sdcard
