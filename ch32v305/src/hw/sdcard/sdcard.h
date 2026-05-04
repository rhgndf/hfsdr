#pragma once

#include <cstdint>
#include <span>

#include "debug.h"
#include "types.h"

namespace sdcard {

void init();
ErrorStatus detect();
bool detected();
const CID& cid();
Status status();
ErrorStatus read_sector(uint32_t sector, std::span<uint8_t, 512> buf);
ErrorStatus read_sectors(uint32_t start_sector, std::span<uint8_t> buf);

} // namespace sdcard
