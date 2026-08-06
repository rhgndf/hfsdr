#pragma once

#include <cstdint>
#include <expected>
#include <span>

#include "debug.h"
#include "types.h"

namespace sdcard {

void init();
ErrorStatus detect(SwitchStatus switch_status);
bool detected();
auto cid() -> std::expected<CID, ErrorStatus>;
Status status();
ErrorStatus read_sector(uint32_t sector, std::span<uint8_t, 512> buf);
ErrorStatus read_sectors(uint32_t start_sector, std::span<uint8_t> buf);
ErrorStatus write_sector(uint32_t sector, std::span<const uint8_t, 512> buf);
ErrorStatus write_sectors(uint32_t start_sector, std::span<const uint8_t> buf);
ErrorStatus sync();

} // namespace sdcard
