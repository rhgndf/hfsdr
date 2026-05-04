extern "C" {
#include "debug.h"
}

#include "sdcard.h"
#include "sdio.h"
#include "bitbang.h"

#include <concepts>
#include <expected>

namespace sdcard {
namespace {

template<typename T>
concept SDTransport = requires(T t, uint32_t u, std::span<uint8_t> buf) {
    { t.init() };
    { t.detect() } -> std::same_as<std::expected<DetectResult, ErrorStatus>>;
    { t.read_blocks(u, buf) } -> std::same_as<ErrorStatus>;
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

    ErrorStatus read_sector(uint32_t sector, std::span<uint8_t, 512> buf)
    {
        if(!initialized) return NoREADY;
        uint32_t addr = sdhc ? sector : sector * 512U;
        return transport.read_blocks(addr, buf);
    }

    ErrorStatus read_sectors(uint32_t start_sector, std::span<uint8_t> buf)
    {
        if(!initialized || buf.empty() || buf.size() % 512U != 0U)
            return NoREADY;
        if(buf.size() == 512U)
            return read_sector(start_sector, buf.first<512>());
        uint32_t addr = sdhc ? start_sector : start_sector * 512U;
        return transport.read_blocks(addr, buf);
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
ErrorStatus read_sector(uint32_t s, std::span<uint8_t, 512> b)   { return s_sd.read_sector(s, b); }
ErrorStatus read_sectors(uint32_t s, std::span<uint8_t> b)        { return s_sd.read_sectors(s, b); }

} // namespace sdcard
