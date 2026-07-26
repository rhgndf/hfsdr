extern "C" {
#include "debug.h"
}

#include "sdcard.h"
#include "sdio.h"
#include "bitbang.h"

#include <algorithm>
#include <concepts>
#include <expected>
#include <limits>

namespace sdcard {
namespace {

template<typename T>
concept SDTransport = requires(T t, uint32_t u, std::span<uint8_t> buf,
                               std::span<const uint8_t> const_buf) {
    { t.init() };
    { t.detect() } -> std::same_as<std::expected<DetectResult, ErrorStatus>>;
    { t.read_blocks(u, buf) } -> std::same_as<ErrorStatus>;
    { t.write_blocks(u, const_buf) } -> std::same_as<ErrorStatus>;
    { t.sync() } -> std::same_as<ErrorStatus>;
    { T::max_blocks_per_transfer } -> std::convertible_to<std::size_t>;
    { t.status() } -> std::same_as<Status>;
};

template<SDTransport Transport>
class SDCard {
public:
    void init() { transport.init(); }

    ErrorStatus detect()
    {
        initialized = false;
        auto result = transport.detect();
        if(!result) return NoREADY;
        card_cid = result->cid;
        sdhc = result->sdhc;
        initialized = true;
        return READY;
    }

    bool detected() const { return initialized; }
    const CID& get_cid() const { return card_cid; }
    Status status() const
    {
        auto s = transport.status();
        s.detected = initialized;
        return s;
    }

    ErrorStatus read_sector(uint32_t sector, std::span<uint8_t, 512> buf)
    {
        return read_sectors(sector, buf);
    }

    ErrorStatus read_sectors(uint32_t start_sector, std::span<uint8_t> buf)
    {
        if(!initialized || buf.empty() || buf.size() % 512U != 0U)
            return NoREADY;
        uint64_t block_count = buf.size() / 512U;
        if(block_count > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) -
                         start_sector + 1U)
            return NoREADY;
        if(!sdhc && (start_sector + block_count - 1U) >
                    std::numeric_limits<uint32_t>::max() / 512U)
            return NoREADY;

        std::size_t block_offset = 0U;
        while(block_offset < block_count)
        {
            uint32_t sector = start_sector + static_cast<uint32_t>(block_offset);
            uint32_t addr = sdhc ? sector : sector * 512U;

            std::size_t blocks_remaining =
                static_cast<std::size_t>(block_count) - block_offset;
            std::size_t blocks_this_transfer =
                std::min(blocks_remaining, Transport::max_blocks_per_transfer);
            std::size_t bytes_this_transfer = blocks_this_transfer * 512U;
            if(transport.read_blocks(
                   addr,
                   buf.subspan(block_offset * 512U, bytes_this_transfer)) != READY)
                return NoREADY;
            block_offset += blocks_this_transfer;
        }

        return READY;
    }

    ErrorStatus write_sector(uint32_t sector, std::span<const uint8_t, 512> buf)
    {
        return write_sectors(sector, buf);
    }

    ErrorStatus write_sectors(uint32_t start_sector, std::span<const uint8_t> buf)
    {
        if(!initialized || buf.empty() || buf.size() % 512U != 0U)
            return NoREADY;

        uint64_t block_count = buf.size() / 512U;
        if(block_count > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) -
                         start_sector + 1U)
            return NoREADY;
        if(!sdhc && (start_sector + block_count - 1U) >
                    std::numeric_limits<uint32_t>::max() / 512U)
            return NoREADY;

        std::size_t block_offset = 0U;
        while(block_offset < block_count)
        {
            uint32_t sector = start_sector + static_cast<uint32_t>(block_offset);
            uint32_t addr = sdhc ? sector : sector * 512U;

            std::size_t blocks_remaining =
                static_cast<std::size_t>(block_count) - block_offset;
            std::size_t blocks_this_transfer =
                std::min(blocks_remaining, Transport::max_blocks_per_transfer);
            std::size_t bytes_this_transfer = blocks_this_transfer * 512U;
            if(transport.write_blocks(
                   addr,
                   buf.subspan(block_offset * 512U, bytes_this_transfer)) != READY)
                return NoREADY;
            block_offset += blocks_this_transfer;
        }

        return READY;
    }

    ErrorStatus sync()
    {
        return initialized ? transport.sync() : NoREADY;
    }

private:
    Transport transport{};
    bool initialized = false;
    bool sdhc = false;
    CID card_cid = {};
};

// --- compile-time transport selection -------------------------------------

using SD = SDCard<SDIOTransport>;
//using SD = SDCard<BitbangTransport>;

SD s_sd;

} // anonymous namespace

// --- public free-function API (delegates to s_sd) -------------------------

void init()                                                      { s_sd.init(); }
ErrorStatus detect()                                             { return s_sd.detect(); }
bool detected()                                                  { return s_sd.detected(); }
const CID& cid()                                                 { return s_sd.get_cid(); }
Status status()                                                  { return s_sd.status(); }
ErrorStatus read_sector(uint32_t s, std::span<uint8_t, 512> b)   { return s_sd.read_sector(s, b); }
ErrorStatus read_sectors(uint32_t s, std::span<uint8_t> b)        { return s_sd.read_sectors(s, b); }
ErrorStatus write_sector(uint32_t s, std::span<const uint8_t, 512> b) { return s_sd.write_sector(s, b); }
ErrorStatus write_sectors(uint32_t s, std::span<const uint8_t> b) { return s_sd.write_sectors(s, b); }
ErrorStatus sync()                                               { return s_sd.sync(); }

} // namespace sdcard
