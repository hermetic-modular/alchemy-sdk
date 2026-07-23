/**
 * @file tests/host/ram_diskio.cpp
 * @brief RAM-backed implementation of the FatFS diskio contract.
 *
 * Replaces libDaisy's diskio.c/ff_gen_drv.c dispatch entirely — ff.c
 * resolves disk_* straight to this file in the host test build.  Only
 * physical drive 0 exists (volume "0:").
 */

#include "ram_diskio.h"

#include <cstring>
#include <vector>

#include "diskio.h"
#include "ff.h"

namespace ramdisk {

namespace {

std::vector<uint8_t> s_image;
bool     s_initialized     = false;
int64_t  s_writes_until_fail = -1;   /* <0 = no fault armed */
bool     s_fail_all        = false;
uint32_t s_write_count     = 0u;

bool Faulted() { return s_fail_all || s_writes_until_fail == 0; }

} // namespace

void Reset(uint32_t sectors)
{
    s_image.assign(size_t(sectors) * kSectorBytes, 0u);
    s_initialized       = false;
    s_writes_until_fail = -1;
    s_fail_all          = false;
    s_write_count       = 0u;
}

uint32_t Sectors() { return static_cast<uint32_t>(s_image.size() / kSectorBytes); }
uint8_t* Image()   { return s_image.data(); }

void FailAfterWrites(int32_t n) { s_writes_until_fail = n; }
void FailAll(bool on)           { s_fail_all = on; }
void ClearFaults()              { s_fail_all = false; s_writes_until_fail = -1; }
uint32_t WriteCount()           { return s_write_count; }

namespace {
uint32_t s_unaligned_io = 0u;
}
uint32_t UnalignedIo() { return s_unaligned_io; }

/** SDMMC's IDMA silently truncates buffer addresses to word alignment —
 *  an unaligned pointer reaching disk I/O shifts real data by 1–3 bytes
 *  on hardware (the REC0001.WAV download bug).  The RAM disk counts
 *  every such pointer so the test suite can assert none ever appear. */
static void NoteAlignment(const void* buff)
{
    if ((reinterpret_cast<uintptr_t>(buff) & 3u) != 0u)
        ramdisk::s_unaligned_io++;
}

} // namespace ramdisk

/* ── FatFS diskio contract ─────────────────────────────────────────────── */

extern "C" {

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv != 0u || ramdisk::s_image.empty() || ramdisk::Faulted())
        return STA_NOINIT;
    ramdisk::s_initialized = true;
    return 0u;
}

DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv != 0u || !ramdisk::s_initialized || ramdisk::Faulted())
        return STA_NOINIT;
    return 0u;
}

DRESULT disk_read(BYTE pdrv, BYTE* buff, DWORD sector, UINT count)
{
    if (pdrv != 0u || !ramdisk::s_initialized) return RES_NOTRDY;
    if (ramdisk::Faulted()) return RES_ERROR;
    ramdisk::NoteAlignment(buff);
    const uint64_t end = (uint64_t(sector) + count) * ramdisk::kSectorBytes;
    if (end > ramdisk::s_image.size()) return RES_PARERR;
    std::memcpy(buff,
                ramdisk::s_image.data() + size_t(sector) * ramdisk::kSectorBytes,
                size_t(count) * ramdisk::kSectorBytes);
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE* buff, DWORD sector, UINT count)
{
    if (pdrv != 0u || !ramdisk::s_initialized) return RES_NOTRDY;
    ramdisk::NoteAlignment(buff);
    for (UINT i = 0u; i < count; i++)
    {
        if (ramdisk::Faulted()) return RES_ERROR;
        const uint64_t end = (uint64_t(sector) + i + 1u) * ramdisk::kSectorBytes;
        if (end > ramdisk::s_image.size()) return RES_PARERR;
        std::memcpy(ramdisk::s_image.data()
                        + (size_t(sector) + i) * ramdisk::kSectorBytes,
                    buff + size_t(i) * ramdisk::kSectorBytes,
                    ramdisk::kSectorBytes);
        ramdisk::s_write_count++;
        if (ramdisk::s_writes_until_fail > 0) ramdisk::s_writes_until_fail--;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff)
{
    if (pdrv != 0u || !ramdisk::s_initialized) return RES_NOTRDY;
    switch (cmd)
    {
        case CTRL_SYNC:
            return ramdisk::Faulted() ? RES_ERROR : RES_OK;
        case GET_SECTOR_COUNT:
            *static_cast<DWORD*>(buff) = ramdisk::Sectors();
            return RES_OK;
        case GET_SECTOR_SIZE:
            *static_cast<WORD*>(buff) = ramdisk::kSectorBytes;
            return RES_OK;
        case GET_BLOCK_SIZE:
            *static_cast<DWORD*>(buff) = 1u;
            return RES_OK;
        default:
            return RES_PARERR;
    }
}

/** Fixed timestamp so mtimes are deterministic in golden vectors:
 *  2026-07-21 12:00:00 → fdate 0x5CF5, ftime 0x6000. */
DWORD get_fattime(void)
{
    return (DWORD(2026u - 1980u) << 25) | (DWORD(7u) << 21) | (DWORD(21u) << 16)
         | (DWORD(12u) << 11);
}

} /* extern "C" */
