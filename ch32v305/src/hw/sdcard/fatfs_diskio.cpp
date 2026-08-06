extern "C" {
#include "ff.h"
#include "diskio.h"
}

#include "sdcard.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace {

constexpr BYTE SD_DRIVE = 0U;
constexpr std::size_t SD_SECTOR_SIZE = 512U;

DSTATUS unavailable_status()
{
    return static_cast<DSTATUS>(STA_NOINIT | STA_NODISK);
}

bool valid_transfer(BYTE pdrv, const void* buffer, LBA_t sector, UINT count)
{
    if(pdrv != SD_DRIVE || buffer == nullptr || count == 0U)
    {
        return false;
    }

    if(static_cast<std::size_t>(count) >
       std::numeric_limits<std::size_t>::max() / SD_SECTOR_SIZE)
    {
        return false;
    }

    uint64_t last_sector = static_cast<uint64_t>(sector) +
                           static_cast<uint64_t>(count) - 1U;
    return last_sector <= std::numeric_limits<uint32_t>::max();
}

DRESULT transfer_result(ErrorStatus result)
{
    if(result == READY)
    {
        return RES_OK;
    }

    return sdcard::detected() ? RES_ERROR : RES_NOTRDY;
}

} // namespace

extern "C" {

DSTATUS disk_status(BYTE pdrv)
{
    if(pdrv != SD_DRIVE)
    {
        return STA_NOINIT;
    }

    return sdcard::detected() ? 0U : unavailable_status();
}

DSTATUS disk_initialize(BYTE pdrv)
{
    if(pdrv != SD_DRIVE)
    {
        return STA_NOINIT;
    }

    return sdcard::detected() ? 0U : unavailable_status();
}

DRESULT disk_read(BYTE pdrv, BYTE* buffer, LBA_t sector, UINT count)
{
    if(!valid_transfer(pdrv, buffer, sector, count))
    {
        return RES_PARERR;
    }
    if(!sdcard::detected())
    {
        return RES_NOTRDY;
    }

    std::size_t byte_count = static_cast<std::size_t>(count) * SD_SECTOR_SIZE;
    return transfer_result(sdcard::read_sectors(
        static_cast<uint32_t>(sector),
        std::span<uint8_t>{buffer, byte_count}));
}

DRESULT disk_write(BYTE pdrv, const BYTE* buffer, LBA_t sector, UINT count)
{
    if(!valid_transfer(pdrv, buffer, sector, count))
    {
        return RES_PARERR;
    }
    if(!sdcard::detected())
    {
        return RES_NOTRDY;
    }

    std::size_t byte_count = static_cast<std::size_t>(count) * SD_SECTOR_SIZE;
    return transfer_result(sdcard::write_sectors(
        static_cast<uint32_t>(sector),
        std::span<const uint8_t>{buffer, byte_count}));
}

DRESULT disk_ioctl(BYTE pdrv, BYTE command, void* buffer)
{
    if(pdrv != SD_DRIVE)
    {
        return RES_PARERR;
    }
    if(!sdcard::detected())
    {
        return RES_NOTRDY;
    }

    switch(command)
    {
    case CTRL_SYNC:
        return transfer_result(sdcard::sync());

    case GET_SECTOR_SIZE:
        if(buffer == nullptr)
        {
            return RES_PARERR;
        }
        *static_cast<WORD*>(buffer) = static_cast<WORD>(SD_SECTOR_SIZE);
        return RES_OK;

    case GET_BLOCK_SIZE:
        if(buffer == nullptr)
        {
            return RES_PARERR;
        }
        *static_cast<DWORD*>(buffer) = 1U;
        return RES_OK;

    default:
        return RES_PARERR;
    }
}

} // extern "C"
