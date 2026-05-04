#pragma once

#include <array>
#include <cstdint>

namespace sdcard {

struct R2 { uint32_t w[4]; };

struct CID {
    uint8_t mid;
    std::array<char, 3> oid;
    std::array<char, 6> pnm;
    uint8_t prv_major;
    uint8_t prv_minor;
    uint32_t psn;
    uint16_t mdt_year;
    uint8_t mdt_month;
};

struct DetectResult {
    CID cid;
    bool sdhc;
};

struct Status {
    bool detected;
    uint8_t bus_width_bits;
    uint32_t clock_hz;
    bool high_speed;
};

inline CID parse_cid(const R2& r)
{
    return {
        .mid       = static_cast<uint8_t>(r.w[0] >> 24),
        .oid       = {static_cast<char>((r.w[0] >> 16) & 0xFF),
                      static_cast<char>((r.w[0] >> 8) & 0xFF), '\0'},
        .pnm       = {static_cast<char>(r.w[0] & 0xFF),
                      static_cast<char>((r.w[1] >> 24) & 0xFF),
                      static_cast<char>((r.w[1] >> 16) & 0xFF),
                      static_cast<char>((r.w[1] >> 8) & 0xFF),
                      static_cast<char>(r.w[1] & 0xFF), '\0'},
        .prv_major = static_cast<uint8_t>((r.w[2] >> 28) & 0x0FU),
        .prv_minor = static_cast<uint8_t>((r.w[2] >> 24) & 0x0FU),
        .psn       = (r.w[2] << 8) | (r.w[3] >> 24),
        .mdt_year  = static_cast<uint16_t>(2000U + ((r.w[3] >> 12) & 0xFFU)),
        .mdt_month = static_cast<uint8_t>((r.w[3] >> 8) & 0x0FU),
    };
}

} // namespace sdcard
